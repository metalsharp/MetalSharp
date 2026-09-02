#include "dxil_to_msl.hpp"
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <cstdlib>
#include <cstdarg>
#include <algorithm>
#include <map>
#include <set>

static FILE *
dxmt_dxil_open_trace_log(const char *fallback_name) {
  const char *root = std::getenv("DXMT_LOG_PATH");
  const char *file = fallback_name && fallback_name[0] ? fallback_name : "dxmt-dxil-trace.log";
  char path[4096];

  if (!root || !root[0])
    return std::fopen(file, "a");

  std::snprintf(path, sizeof(path), "%s%s%s", root, (root[std::strlen(root) - 1] == '/' || root[std::strlen(root) - 1] == '\\') ? "" : "/", file);
  path[sizeof(path) - 1] = '\0';
  return std::fopen(path, "a");
}

#define DXTRACE(fmt, ...) do { FILE *_tf = dxmt_dxil_open_trace_log("dxmt-dxil-trace.log"); if (_tf) { fprintf(_tf, fmt "\n", ##__VA_ARGS__); fclose(_tf); } } while(0)

namespace dxmt::dxil {

enum DXIntrinsicOpcode {
  DXOP_LoadInput = 4,
  DXOP_StoreOutput = 5,
  DXOP_CreateHandle = 57,
  DXOP_CreateHandleForLib = 160,
  DXOP_AnnotateHandle = 216,
  DXOP_CreateHandleFromBinding = 217,
  DXOP_CreateHandleFromHeap = 218,
  DXOP_CBufferLoad = 58,
  DXOP_CBufferLoadLegacy = 59,
  DXOP_ThreadId = 93,
  DXOP_GroupId = 94,
  DXOP_ThreadIDInGroup = 95,
  DXOP_FlattenedThreadIDInGroup = 96,
  DXOP_BufferLoad = 68,
  DXOP_BufferStore = 69,
  DXOP_TextureLoad = 66,
  DXOP_TextureStore = 67,
  DXOP_TextureStoreSample = 225,
  DXOP_TextureGather = 73,
  DXOP_TextureGatherCmp = 74,
  DXOP_TextureGatherRaw = 223,
  DXOP_TextureSample = 60,
  DXOP_TextureSampleBias = 61,
  DXOP_TextureSampleLevel = 62,
  DXOP_TextureSampleGrad = 63,
  DXOP_TextureSampleCmp = 64,
  DXOP_TextureSampleCmpLevelZero = 65,
  DXOP_TextureSampleCmpLevel = 224,
  DXOP_BufferUpdateCounter = 70,
  DXOP_CheckAccessFullyMapped = 71,
  DXOP_GetDimensions = 72,
  DXOP_Barrier = 80,
  DXOP_Unary = 13,
  DXOP_Binary = 14,
  DXOP_Tertiary = 15,
  DXOP_Dot2 = 54,
  DXOP_Dot3 = 55,
  DXOP_Dot4 = 56,
  DXOP_MakeDouble = 101,
  DXOP_SplitDouble = 102,
  DXOP_RawBufferLoad = 139,
  DXOP_RawBufferStore = 140,
  DXOP_RawBufferVectorLoad = 303,
  DXOP_RawBufferVectorStore = 304,
  DXOP_RawBufferLoadLegacy = 1025,
  DXOP_RawBufferStoreLegacy = 1026,
  DXOP_AtomicBinOp = 78,
  DXOP_AtomicCompareExchange = 79,
  DXOP_DerivCoarseX = 83,
  DXOP_DerivCoarseY = 84,
  DXOP_DerivFineX = 85,
  DXOP_DerivFineY = 86,
  DXOP_CalcLOD = 81,
  DXOP_Texture2DMSGetSamplePosition = 75,
  DXOP_RenderTargetGetSamplePosition = 76,
  DXOP_RenderTargetGetSampleCount = 77,
  DXOP_Texture2DMSGetSamplePositionLegacy = 97,
  DXOP_RenderTargetGetSamplePositionLegacy = 98,
  DXOP_CycleCounterLegacy = 109,
  DXOP_WaveIsFirstLane = 110,
  DXOP_WaveGetLaneIndex = 111,
  DXOP_WaveGetLaneCount = 112,
  DXOP_WaveAnyTrue = 113,
  DXOP_WaveAllTrue = 114,
  DXOP_WaveReadLaneAt = 117,
  DXOP_WaveReadLaneFirst = 118,
  DXOP_QuadReadLaneAt = 122,
  DXOP_LegacyF16ToF32 = 131,
  DXOP_LegacyF32ToF16 = 132,
  // HitObject and SER operations intentionally use an internal sentinel.
  // Their DXIL opcode is carried by the first call operand and is passed
  // separately to translateDXIntrinsic for fail-closed diagnostics.
  DXOP_HitObjectOrSER = 1100,
};

enum DXILMathOpcode {
  DXILOP_FAbs = 6,
  DXILOP_Saturate = 7,
  DXILOP_IsNaN = 8,
  DXILOP_IsInf = 9,
  DXILOP_IsFinite = 10,
  DXILOP_Cos = 12,
  DXILOP_Sin = 13,
  DXILOP_Tan = 14,
  DXILOP_Acos = 15,
  DXILOP_Asin = 16,
  DXILOP_Atan = 17,
  DXILOP_Exp = 21,
  DXILOP_Frc = 22,
  DXILOP_Log = 23,
  DXILOP_Sqrt = 24,
  DXILOP_Rsqrt = 25,
  DXILOP_Round_ne = 26,
  DXILOP_Round_ni = 27,
  DXILOP_Round_pi = 28,
  DXILOP_Round_z = 29,
  DXILOP_Bfrev = 30,
  DXILOP_Countbits = 31,
  DXILOP_FirstbitLo = 32,
  DXILOP_FirstbitHi = 33,
  DXILOP_FirstbitSHi = 34,
  DXILOP_FMax = 35,
  DXILOP_FMin = 36,
  DXILOP_IMax = 37,
  DXILOP_IMin = 38,
  DXILOP_UMax = 39,
  DXILOP_UMin = 40,
  DXILOP_IMul = 41,
  DXILOP_UMul = 42,
  DXILOP_UDiv = 43,
  DXILOP_UAddc = 44,
  DXILOP_USubb = 45,
  DXILOP_FMad = 46,
  DXILOP_Fma = 47,
  DXILOP_IMad = 48,
  DXILOP_UMad = 49,
  DXILOP_Msad = 50,
  DXILOP_Ibfe = 51,
  DXILOP_Ubfe = 52,
  DXILOP_Bfi = 53,
};

static const char *kMetalHeader = R"(#include <metal_stdlib>
using namespace metal;

)";

static std::string escapeName(const std::string &s) {
  if (s.empty()) return "_";
  std::string r;
  for (char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_')
      r += c;
    else
      r += '_';
  }
  if (!r.empty() && r[0] >= '0' && r[0] <= '9')
    r = "_" + r;
  return r;
}

static const char *componentSuffix(uint32_t component) {
  switch (component & 3) {
  case 0: return ".x";
  case 1: return ".y";
  case 2: return ".z";
  default: return ".w";
  }
}

static const char *componentName(uint32_t component) {
  switch (component & 3) {
  case 0: return "x";
  case 1: return "y";
  case 2: return "z";
  default: return "w";
  }
}

static std::string varyingField(const char *base, uint32_t signature_id) {
  switch (signature_id) {
  case 0: return std::string(base) + ".position";
  case 1: return std::string(base) + ".v0";
  case 2: return std::string(base) + ".v1";
  case 3: return std::string(base) + ".v2";
  case 4: return std::string(base) + ".v3";
  case 5: return std::string(base) + ".v4";
  case 6: return std::string(base) + ".v5";
  case 7: return std::string(base) + ".v6";
  case 8: return std::string(base) + ".v7";
  default: return std::string(base) + ".v0";
  }
}

static std::string vertexInputField(const char *base, uint32_t signature_id) {
  switch (signature_id) {
  case 0: return std::string(base) + ".a0";
  case 1: return std::string(base) + ".a1";
  case 2: return std::string(base) + ".a2";
  case 3: return std::string(base) + ".a3";
  case 4: return std::string(base) + ".a4";
  case 5: return std::string(base) + ".a5";
  case 6: return std::string(base) + ".a6";
  case 7: return std::string(base) + ".a7";
  case 8: return std::string(base) + ".a8";
  case 9: return std::string(base) + ".a9";
  case 10: return std::string(base) + ".a10";
  case 11: return std::string(base) + ".a11";
  case 12: return std::string(base) + ".a12";
  case 13: return std::string(base) + ".a13";
  case 14: return std::string(base) + ".a14";
  case 15: return std::string(base) + ".a15";
  default: return std::string(base) + ".a0";
  }
}

static bool parseUnsignedLiteral(const std::string &text, uint32_t &value) {
  if (text.empty())
    return false;
  char *end = nullptr;
  unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (!end || *end != '\0')
    return false;
  value = (uint32_t)parsed;
  return true;
}

static std::string componentAccessor(const std::string &index) {
  uint32_t component = 0;
  if (parseUnsignedLiteral(index, component))
    return componentSuffix(component);
  return "[" + index + "]";
}

static bool startsWith(const std::string &text, const char *prefix) {
  return text.rfind(prefix, 0) == 0;
}

static std::vector<std::string> parseAggregateLiteral(const std::string &text) {
  std::vector<std::string> values;
  bool is_agg = startsWith(text, "agg(") && text.size() >= 5 && text.back() == ')';
  bool is_brace = !text.empty() && text[0] == '{' && text.back() == '}';
  if (!is_agg && !is_brace)
    return values;
  size_t start = is_agg ? 4 : 1;
  while (start < text.size() - 1) {
    size_t comma = text.find(',', start);
    size_t end = comma == std::string::npos ? text.size() - 1 : comma;
    std::string val = text.substr(start, end - start);
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
      val.erase(val.begin());
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
      val.pop_back();
    if (!val.empty())
      values.push_back(val);
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return values;
}

static bool isZeroLiteral(const std::string &text) {
  return text == "0" || text == "0.0" || text == "0.0f" || text == "false";
}

static std::string ensureScalarIndex(const std::string &val) {
  if (startsWith(val, "agg(")) {
    auto parts = parseAggregateLiteral(val);
    return parts.empty() ? "0" : parts[0];
  }
  return val;
}

static std::string vectorComponent(const std::string &vector, uint32_t index) {
  return "(" + vector + ")" + componentSuffix(index);
}

static const char *bindingPrefixForClass(uint32_t resource_class) {
  switch (resource_class) {
  case 0: return "tex";
  case 1: return "tex";
  case 2: return "buf";
  case 3: return "samp";
  default: return "buf";
  }
}

static std::string resolveBindingName(const std::string &handle, const char *target_prefix) {
  if (startsWith(handle, "agg(")) {
    auto parts = parseAggregateLiteral(handle);
    uint32_t lower_bound = 0;
    if (parts.size() > 0) parseUnsignedLiteral(parts[0], lower_bound);
    return std::string(target_prefix) + std::to_string(lower_bound);
  }
  const char *prefixes[] = {"tex", "buf", "samp"};
  for (auto *prefix : prefixes) {
    if (startsWith(handle, prefix)) {
      const char *suffix = handle.c_str() + std::strlen(prefix);
      return std::string(target_prefix) + suffix;
    }
  }
  return handle;
}

static uint32_t intrinsicIdFromCalleeName(const std::string &name) {
  if (name.size() < 6 || name[0] != 'd' || name[1] != 'x' || name[2] != '.' || name[3] != 'o' || name[4] != 'p' || name[5] != '.')
    return 0;
  const char *s = name.c_str() + 6;
  if (strncmp(s, "loadInput.", 10) == 0) return DXOP_LoadInput;
  if (strncmp(s, "storeOutput.", 12) == 0) return DXOP_StoreOutput;
  if (strncmp(s, "createHandleFromBinding", 23) == 0) return DXOP_CreateHandleFromBinding;
  if (strncmp(s, "createHandleFromHeap", 20) == 0) return DXOP_CreateHandleFromHeap;
  if (strncmp(s, "createHandleForLib", 18) == 0) return DXOP_CreateHandleForLib;
  if (strncmp(s, "createHandle", 12) == 0) return DXOP_CreateHandle;
  if (strncmp(s, "annotateHandle", 14) == 0) return DXOP_AnnotateHandle;
  if (strncmp(s, "cbufferLoadLegacy.", 18) == 0) return DXOP_CBufferLoadLegacy;
  if (strncmp(s, "cbufferLoad.", 12) == 0) return DXOP_CBufferLoad;
  if (strncmp(s, "threadIdInGroup", 15) == 0) return DXOP_ThreadIDInGroup;
  if (strncmp(s, "flattenedThreadIdInGroup", 24) == 0) return DXOP_FlattenedThreadIDInGroup;
  if (strncmp(s, "threadId", 8) == 0) return DXOP_ThreadId;
  if (strncmp(s, "groupId", 7) == 0) return DXOP_GroupId;
  if (strncmp(s, "bufferLoad.", 11) == 0) return DXOP_BufferLoad;
  if (strncmp(s, "bufferStore.", 12) == 0) return DXOP_BufferStore;
  if (strncmp(s, "bufferUpdateCounter", 19) == 0) return DXOP_BufferUpdateCounter;
  if (strncmp(s, "textureStoreSample.", 19) == 0) return DXOP_TextureStoreSample;
  if (strncmp(s, "textureStore.", 13) == 0) return DXOP_TextureStore;
  if (strncmp(s, "textureLoad.", 12) == 0) return DXOP_TextureLoad;
  if (strncmp(s, "textureGatherCmp.", 17) == 0) return DXOP_TextureGatherCmp;
  if (strncmp(s, "textureGatherRaw.", 17) == 0) return DXOP_TextureGatherRaw;
  if (strncmp(s, "textureGather.", 14) == 0) return DXOP_TextureGather;
  if (strncmp(s, "sampleCmpLevelZero.", 19) == 0) return DXOP_TextureSampleCmpLevelZero;
  if (strncmp(s, "sampleCmpLevel.", 15) == 0) return DXOP_TextureSampleCmpLevel;
  if (strncmp(s, "sampleCmp.", 10) == 0) return DXOP_TextureSampleCmp;
  if (strncmp(s, "sampleGrad.", 11) == 0) return DXOP_TextureSampleGrad;
  if (strncmp(s, "sampleLevel.", 12) == 0) return DXOP_TextureSampleLevel;
  if (strncmp(s, "sampleBias.", 10) == 0) return DXOP_TextureSampleBias;
  if (strncmp(s, "sample.", 7) == 0) return DXOP_TextureSample;
  if (strncmp(s, "unary.", 6) == 0) return DXOP_Unary;
  if (strncmp(s, "binary.", 7) == 0) return DXOP_Binary;
  if (strncmp(s, "tertiary.", 9) == 0) return DXOP_Tertiary;
  if (strncmp(s, "dot2.", 5) == 0) return DXOP_Dot2;
  if (strncmp(s, "dot3.", 5) == 0) return DXOP_Dot3;
  if (strncmp(s, "dot4.", 5) == 0) return DXOP_Dot4;
  if (strncmp(s, "barrier", 7) == 0) return DXOP_Barrier;
  if (strncmp(s, "checkAccessFullyMapped", 22) == 0) return DXOP_CheckAccessFullyMapped;
  if (strncmp(s, "getDimensions", 13) == 0) return DXOP_GetDimensions;
  if (strncmp(s, "rawBufferLoadLegacy", 19) == 0) return DXOP_RawBufferLoadLegacy;
  if (strncmp(s, "rawBufferStoreLegacy", 20) == 0) return DXOP_RawBufferStoreLegacy;
  if (strncmp(s, "rawBufferVectorLoad", 19) == 0) return DXOP_RawBufferVectorLoad;
  if (strncmp(s, "rawBufferVectorStore", 20) == 0) return DXOP_RawBufferVectorStore;
  if (strncmp(s, "rawBufferLoad", 13) == 0) return DXOP_RawBufferLoad;
  if (strncmp(s, "rawBufferStore", 14) == 0) return DXOP_RawBufferStore;
  if (strncmp(s, "atomicCompareExchange", 21) == 0) return DXOP_AtomicCompareExchange;
  if (strncmp(s, "atomicBinOp", 11) == 0) return DXOP_AtomicBinOp;
  if (strncmp(s, "derivCoarseX", 12) == 0) return DXOP_DerivCoarseX;
  if (strncmp(s, "derivCoarseY", 12) == 0) return DXOP_DerivCoarseY;
  if (strncmp(s, "derivFineX", 10) == 0) return DXOP_DerivFineX;
  if (strncmp(s, "derivFineY", 10) == 0) return DXOP_DerivFineY;
  if (strncmp(s, "calculateLOD", 12) == 0 || strncmp(s, "calcLOD", 7) == 0) return DXOP_CalcLOD;
  if (strncmp(s, "makeDouble", 10) == 0) return DXOP_MakeDouble;
  if (strncmp(s, "splitDouble", 11) == 0) return DXOP_SplitDouble;
  if (strncmp(s, "legacyF16ToF32", 14) == 0) return DXOP_LegacyF16ToF32;
  if (strncmp(s, "legacyF32ToF16", 14) == 0) return DXOP_LegacyF32ToF16;
  if (strncmp(s, "waveReadLaneFirst", 17) == 0) return DXOP_WaveReadLaneFirst;
  if (strncmp(s, "waveReadLaneAt", 14) == 0) return DXOP_WaveReadLaneAt;
  if (strncmp(s, "waveIsFirstLane", 15) == 0) return DXOP_WaveIsFirstLane;
  if (strncmp(s, "waveGetLaneIndex", 16) == 0) return DXOP_WaveGetLaneIndex;
  if (strncmp(s, "waveGetLaneCount", 16) == 0) return DXOP_WaveGetLaneCount;
  if (strncmp(s, "waveAnyTrue", 11) == 0) return DXOP_WaveAnyTrue;
  if (strncmp(s, "waveAllTrue", 11) == 0) return DXOP_WaveAllTrue;
  if (strncmp(s, "quadReadLaneAt", 14) == 0) return DXOP_QuadReadLaneAt;
  if (strncmp(s, "sample.", 7) == 0) return DXOP_TextureSample;
  if (strncmp(s, "textureLoad.", 12) == 0) return DXOP_TextureLoad;
  if (strncmp(s, "threadIdInGroup", 15) == 0) return DXOP_ThreadIDInGroup;
  if (strncmp(s, "unaryBits.", 10) == 0) return DXOP_Unary;
  if (strncmp(s, "isSpecialFloat", 14) == 0) return 0;
  if (strncmp(s, "hitObject_", 10) == 0 ||
      strncmp(s, "maybeReorderThread", 18) == 0)
    return DXOP_HitObjectOrSER;
  if (strncmp(s, "cycleCounterLegacy", 18) == 0) return DXOP_CycleCounterLegacy;
  return 0;
}

void DXILToMSL::recordDiagnostic(EmitContext &ctx, const char *fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  ctx.diagnostics.emplace_back(buffer);
  DXTRACE("%s", buffer);
}

std::string DXILToMSL::getTypeName(const LLVMType &t, const LLVMModule &mod) {
  switch (t.kind) {
  case LLVMType::Void: return "void";
  case LLVMType::Float: return "float";
  case LLVMType::Double: return "float64_t";
  case LLVMType::Integer:
    if (t.bit_width == 1) return "bool";
    if (t.bit_width == 8) return "char";
    if (t.bit_width == 16) return "short";
    if (t.bit_width == 32) return "int";
    if (t.bit_width == 64) return "long";
    return "int";
  case LLVMType::Pointer: return "device char*";
  case LLVMType::Struct: return "char" + std::to_string((uint64_t)&t % 997);
  case LLVMType::Array: return "array<char," + std::to_string(t.bit_width) + ">";
  case LLVMType::Vector: {
    if (t.subtypes.empty())
      return "float4";
    return getTypeName(t.subtypes[0], mod) + std::to_string(t.bit_width);
  }
  case LLVMType::Function: return "void";
  }
  return "int";
}

std::string DXILToMSL::getVectorTypeName(const LLVMType &elem, uint32_t count, const LLVMModule &mod) {
  return getTypeName(elem, mod) + std::to_string(count);
}

uint32_t DXILToMSL::getTypeSize(const LLVMType &t, const LLVMModule &mod) {
  switch (t.kind) {
  case LLVMType::Void: return 0;
  case LLVMType::Float: return 4;
  case LLVMType::Double: return 8;
  case LLVMType::Integer: return (t.bit_width + 7) / 8;
  case LLVMType::Pointer: return 8;
  case LLVMType::Struct: {
    uint32_t s = 0;
    for (auto &st : t.subtypes)
      s += getTypeSize(st, mod);
    return s;
  }
  case LLVMType::Array: return t.bit_width * (t.subtypes.empty() ? 4 : getTypeSize(t.subtypes[0], mod));
  case LLVMType::Vector: return t.bit_width * 4;
  case LLVMType::Function: return 0;
  }
  return 4;
}

std::string DXILToMSL::emitValue(uint32_t idx) {
  if (idx == 0xFFFFFFFF) return "undef";
  return "v" + std::to_string(idx);
}

std::string DXILToMSL::emitConstant(const std::vector<uint64_t> &ops, uint32_t type_id, const LLVMModule &mod) {
  if (type_id >= mod.types.size())
    return "0";
  auto &t = mod.types[type_id];
  if (ops.empty())
    return "0";
  switch (t.kind) {
  case LLVMType::Integer:
    if (t.bit_width == 1) return ops[0] ? "true" : "false";
    if (t.bit_width <= 32) return std::to_string((int32_t)ops[0]);
    return std::to_string((int64_t)ops[0]);
  case LLVMType::Float: {
    float f;
    uint32_t u = (uint32_t)ops[0];
    memcpy(&f, &u, 4);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.9g", (double)f);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E'))
      strcat(buf, ".0");
    return std::string(buf) + "f";
  }
  case LLVMType::Double: {
    double d;
    uint64_t u = ops[0];
    memcpy(&d, &u, 8);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", d);
    return std::string(buf);
  }
  default:
    return "0";
  }
}

void DXILToMSL::emitBindings(EmitContext &ctx) {
  auto &os = ctx.os;

  if (ctx.shader.kind == DxilShaderKind::Compute) {
    os << "  uint3 dtid [[thread_position_in_grid]];\n";
    os << "  uint3 gtid [[thread_position_in_threadgroup]];\n";
    os << "  uint3 ggid [[threadgroup_position_in_grid]];\n";
    os << "  uint3 gsz [[threads_per_threadgroup]];\n";
    ctx.uses_thread_id = true;
    ctx.uses_group_id = true;
    ctx.uses_group_thread_id = true;
    ctx.uses_group_size = true;
  }

  os << "\n";
}

void DXILToMSL::emitFunctionPrologue(EmitContext &ctx) {
  auto &os = ctx.os;
  os << kMetalHeader;

  os << "struct input_v {\n";
  os << "  float4 position [[position]];\n";
  os << "  float4 v0 [[user(locn0)]]; float4 v1 [[user(locn1)]];\n";
  os << "  float4 v2 [[user(locn2)]]; float4 v3 [[user(locn3)]];\n";
  os << "  float4 v4 [[user(locn4)]]; float4 v5 [[user(locn5)]];\n";
  os << "  float4 v6 [[user(locn6)]]; float4 v7 [[user(locn7)]];\n";
  os << "  float2 uv0 [[user(locn8)]]; float2 uv1 [[user(locn9)]];\n";
  os << "  float2 uv2 [[user(locn10)]]; float2 uv3 [[user(locn11)]];\n";
  os << "  float4 color0 [[user(locn12)]]; float4 color1 [[user(locn13)]];\n";
  os << "  float4 color2 [[user(locn14)]]; float4 color3 [[user(locn15)]];\n";
  os << "};\n\n";

  os << "struct output_v {\n";
  os << "  float4 position [[position]];\n";
  os << "  float4 v0 [[user(locn0)]]; float4 v1 [[user(locn1)]];\n";
  os << "  float4 v2 [[user(locn2)]]; float4 v3 [[user(locn3)]];\n";
  os << "  float4 v4 [[user(locn4)]]; float4 v5 [[user(locn5)]];\n";
  os << "  float4 v6 [[user(locn6)]]; float4 v7 [[user(locn7)]];\n";
  os << "  float2 uv0 [[user(locn8)]]; float2 uv1 [[user(locn9)]];\n";
  os << "  float2 uv2 [[user(locn10)]]; float2 uv3 [[user(locn11)]];\n";
  os << "  float4 color0 [[user(locn12)]]; float4 color1 [[user(locn13)]];\n";
  os << "  float4 color2 [[user(locn14)]]; float4 color3 [[user(locn15)]];\n";
  os << "};\n\n";

  auto emit_default_vertex_varyings = [&]() {
    os << "  out.position = float4(0.0, 0.0, 0.0, 1.0);\n";
    for (uint32_t i = 0; i < 8; i++)
      os << "  out.v" << i << " = float4(0.0);\n";
    for (uint32_t i = 0; i < 4; i++)
      os << "  out.uv" << i << " = float2(0.0);\n";
    for (uint32_t i = 0; i < 4; i++)
      os << "  out.color" << i << " = float4(0.0);\n";
  };

  os << "struct vertex_input_v {\n";
  for (uint32_t i = 0; i < 16; i++)
    os << "  float4 a" << i << " [[attribute(" << i << ")]];\n";
  os << "};\n\n";

  if (ctx.shader.kind == DxilShaderKind::Compute) {
    os << "kernel void cs_main(\n";
    os << "  device char* buf0 [[buffer(0)]],\n";
    os << "  device char* buf1 [[buffer(1)]],\n";
    os << "  device char* buf2 [[buffer(2)]],\n";
    os << "  device char* buf3 [[buffer(3)]],\n";
    os << "  device char* buf4 [[buffer(4)]],\n";
    os << "  device char* buf5 [[buffer(5)]],\n";
    os << "  device char* buf6 [[buffer(6)]],\n";
    os << "  device char* buf7 [[buffer(7)]],\n";
    os << "  texture2d<float, access::read_write> tex0 [[texture(0)]],\n";
    os << "  texture2d<float, access::read_write> tex1 [[texture(1)]],\n";
    os << "  texture2d<float, access::read_write> tex2 [[texture(2)]],\n";
    os << "  texture2d<float, access::read_write> tex3 [[texture(3)]],\n";
    os << "  texture2d<float, access::read_write> tex4 [[texture(4)]],\n";
    os << "  texture2d<float, access::read_write> tex5 [[texture(5)]],\n";
    os << "  texture2d<float, access::read_write> tex6 [[texture(6)]],\n";
    os << "  texture2d<float, access::read_write> tex7 [[texture(7)]],\n";
    os << "  sampler samp0 [[sampler(0)]],\n";
    os << "  sampler samp1 [[sampler(1)]],\n";
    os << "  sampler samp2 [[sampler(2)]],\n";
    os << "  sampler samp3 [[sampler(3)]],\n";
    os << "  uint3 dtid [[thread_position_in_grid]],\n";
    os << "  uint3 gtid [[thread_position_in_threadgroup]],\n";
    os << "  uint3 ggid [[threadgroup_position_in_grid]],\n";
    os << "  uint3 gsz [[threads_per_threadgroup]]\n";
    os << ") {\n";
  } else if (ctx.shader.kind == DxilShaderKind::Vertex) {
    os << "vertex output_v vs_main(\n";
    os << "  vertex_input_v vin [[stage_in]],\n";
    os << "  uint vid [[vertex_id]],\n";
    os << "  device char* buf0 [[buffer(0)]],\n";
    os << "  device char* buf1 [[buffer(1)]],\n";
    os << "  device char* buf2 [[buffer(2)]],\n";
    os << "  device char* buf3 [[buffer(3)]],\n";
    os << "  device char* buf4 [[buffer(4)]],\n";
    os << "  device char* buf5 [[buffer(5)]],\n";
    os << "  device char* buf6 [[buffer(6)]],\n";
    os << "  device char* buf7 [[buffer(7)]]\n";
    os << ") {\n";
    os << "  output_v out = {};\n";
    emit_default_vertex_varyings();
  } else if (ctx.shader.kind == DxilShaderKind::Pixel) {
    os << "fragment float4 ps_main(\n";
    os << "  input_v in [[stage_in]],\n";
    os << "  device char* buf0 [[buffer(0)]],\n";
    os << "  device char* buf1 [[buffer(1)]],\n";
    os << "  device char* buf2 [[buffer(2)]],\n";
    os << "  device char* buf3 [[buffer(3)]],\n";
    os << "  device char* buf4 [[buffer(4)]],\n";
    os << "  device char* buf5 [[buffer(5)]],\n";
    os << "  device char* buf6 [[buffer(6)]],\n";
    os << "  device char* buf7 [[buffer(7)]],\n";
    os << "  texture2d<float, access::sample> tex0 [[texture(0)]],\n";
    os << "  texture2d<float, access::sample> tex1 [[texture(1)]],\n";
    os << "  texture2d<float, access::sample> tex2 [[texture(2)]],\n";
    os << "  texture2d<float, access::sample> tex3 [[texture(3)]],\n";
    os << "  texture2d<float, access::sample> tex4 [[texture(4)]],\n";
    os << "  texture2d<float, access::sample> tex5 [[texture(5)]],\n";
    os << "  texture2d<float, access::sample> tex6 [[texture(6)]],\n";
    os << "  texture2d<float, access::sample> tex7 [[texture(7)]],\n";
    os << "  sampler samp0 [[sampler(0)]],\n";
    os << "  sampler samp1 [[sampler(1)]],\n";
    os << "  sampler samp2 [[sampler(2)]],\n";
    os << "  sampler samp3 [[sampler(3)]]\n";
    os << ") {\n";
    os << "  float4 result = float4(0,0,0,1);\n";
  } else {
    os << "kernel void unknown_main() {\n";
  }
}

std::string DXILToMSL::translateDXIntrinsic(EmitContext &ctx, uint32_t intrinsic_id,
                                              const std::vector<uint32_t> &args,
                                              uint32_t explicit_opcode) {
  auto valueArg = [&](size_t arg, const char *fallback) -> std::string {
    if (arg >= args.size())
      return fallback;
    uint32_t idx = args[arg];
    if (idx < ctx.value_table.size() && !ctx.value_table[idx].empty()) {
      const auto &v = ctx.value_table[idx];
      if (v.find('.') != std::string::npos) return fallback;
      return v;
    }
    return fallback;
  };

  auto handleArg = [&](size_t arg, const char *prefix, const char *fallback) -> std::string {
    if (arg >= args.size())
      return fallback;
    uint32_t idx = args[arg];
    auto it = ctx.buffer_origin.find(idx);
    if (it != ctx.buffer_origin.end())
      return it->second;
    return resolveBindingName(valueArg(arg, fallback), prefix);
  };

  auto literalArg = [&](size_t arg, uint32_t fallback, const char *label) -> uint32_t {
    if (arg >= args.size())
      return fallback;
    uint32_t idx = args[arg];
    std::string text;
    if (idx < ctx.value_table.size() && !ctx.value_table[idx].empty())
      text = ctx.value_table[idx];
    else {
      for (auto &c : ctx.mod.constants) {
        if (c.id == idx && !c.constant_data.empty()) { text = c.constant_data; break; }
      }
      if (text.empty() && ctx.current_fn) {
        for (auto &c : ctx.current_fn->constants) {
          if (c.id == idx && !c.constant_data.empty()) { text = c.constant_data; break; }
        }
      }
    }
    uint32_t value = 0;
    if (parseUnsignedLiteral(text, value))
      return value;
    recordDiagnostic(ctx, "DXIL intrinsic %u: %s is not a literal: %s",
                     intrinsic_id, label, text.empty() ? "<missing>" : text.c_str());
    return fallback;
  };

  switch (intrinsic_id) {
  case DXOP_CreateHandle: {
    if (args.size() < 4) return "0";
    uint32_t resource_class = literalArg(0, 0, "resource class");
    uint32_t range_id = literalArg(1, 0, "range id");
    uint32_t index = literalArg(2, 0, "resource index");
    bool non_uniform = literalArg(3, 0, "non-uniform index") != 0;
    (void)non_uniform;
    ctx.next_binding++;
    (void)range_id;
    std::string res_name =
        std::string(bindingPrefixForClass(resource_class)) +
        std::to_string(index);
    DXTRACE("DXIL CreateHandle: class=%u range=%u index=%u -> %s", resource_class, range_id, index, res_name.c_str());
    return res_name;
  }

  case DXOP_CreateHandleForLib: {
    auto handle = valueArg(0, "tex0");
    if (startsWith(handle, "agg("))
      handle = resolveBindingName(handle, "buf");
    DXTRACE("DXIL CreateHandleForLib: %s", handle.c_str());
    return handle;
  }

  case DXOP_AnnotateHandle: {
    auto handle = valueArg(0, "tex0");
    if (startsWith(handle, "agg("))
      handle = resolveBindingName(handle, "buf");
    DXTRACE("DXIL AnnotateHandle: %s", handle.c_str());
    return handle;
  }

  case DXOP_CreateHandleFromBinding: {
    auto binding = valueArg(0, "");
    auto binding_values = parseAggregateLiteral(binding);
    uint32_t lower_bound = 0;
    uint32_t resource_class = 0;
    if (binding_values.size() > 0)
      parseUnsignedLiteral(binding_values[0], lower_bound);
    if (binding_values.size() > 3)
      parseUnsignedLiteral(binding_values[3], resource_class);
    uint32_t index = args.size() >= 2
                         ? literalArg(1, 0, "binding resource index")
                         : 0;
    bool non_uniform = args.size() >= 3 &&
                       literalArg(2, 0, "binding non-uniform index") != 0;
    (void)non_uniform;
    uint32_t binding_index = lower_bound + index;
    std::string res_name =
        std::string(bindingPrefixForClass(resource_class)) +
        std::to_string(binding_index);
    DXTRACE("DXIL CreateHandleFromBinding: binding=%s lower=%u class=%u index=%u -> %s",
            binding.empty() ? "<missing>" : binding.c_str(), lower_bound,
            resource_class, index, res_name.c_str());
    return res_name;
  }

  case DXOP_CreateHandleFromHeap: {
    uint32_t heap_index = literalArg(0, 0, "heap index");
    bool sampler_heap = args.size() >= 2 &&
                        literalArg(1, 0, "sampler heap") != 0;
    bool non_uniform = args.size() >= 3 &&
                       literalArg(2, 0, "heap non-uniform index") != 0;
    (void)non_uniform;
    std::string res_name = std::string(sampler_heap ? "samp" : "tex") +
                           std::to_string(heap_index);
    DXTRACE("DXIL CreateHandleFromHeap: heap_index=%u sampler=%d -> %s",
            heap_index, sampler_heap, res_name.c_str());
    return res_name;
  }

  case DXOP_ThreadId: {
    ctx.uses_thread_id = true;
    if (!args.empty()) {
      uint32_t component = literalArg(0, 0, "thread id component");
      if (component == 0) return "(int)dtid.x";
      if (component == 1) return "(int)dtid.y";
      if (component == 2) return "(int)dtid.z";
    }
    return "(int)dtid.x";
  }

  case DXOP_GroupId: {
    ctx.uses_group_id = true;
    if (!args.empty()) {
      uint32_t component = literalArg(0, 0, "group id component");
      if (component == 0) return "(int)ggid.x";
      if (component == 1) return "(int)ggid.y";
      if (component == 2) return "(int)ggid.z";
    }
    return "(int)ggid.x";
  }

  case DXOP_ThreadIDInGroup: {
    ctx.uses_group_thread_id = true;
    if (!args.empty()) {
      uint32_t component = literalArg(0, 0, "group thread id component");
      if (component == 0) return "(int)gtid.x";
      if (component == 1) return "(int)gtid.y";
      if (component == 2) return "(int)gtid.z";
    }
    return "(int)gtid.x";
  }

  case DXOP_FlattenedThreadIDInGroup: {
    ctx.uses_group_thread_id = true;
    ctx.uses_group_size = true;
    return "(int)(gtid.x + gtid.y * gsz.x + gtid.z * gsz.x * gsz.y)";
  }

  case DXOP_CBufferLoad:
  case DXOP_CBufferLoadLegacy: {
    if (args.size() < 2) return "float4(0)";
    auto handle = handleArg(0, "buf", "buf0");
    ctx.last_buffer_handle = handle;
    auto reg_idx = ensureScalarIndex(valueArg(1, "0"));
    return "(reinterpret_cast<device float4&>(" + handle + "[(" + reg_idx + ")*64]))";
  }

  case DXOP_BufferLoad: {
    if (args.size() < 3) return "float4(0)";
    auto handle = handleArg(0, "buf", "tex0");
    ctx.last_buffer_handle = handle;
    auto index = ensureScalarIndex(valueArg(1, "0"));
    return "(reinterpret_cast<device float4&>(" + handle + "[(" + index + ")*16]))";
  }

  case DXOP_RawBufferLoad:
  case DXOP_RawBufferVectorLoad:
  case DXOP_RawBufferLoadLegacy: {
    if (args.size() < 3) return "uint4(0)";
    auto handle = handleArg(0, "buf", "tex0");
    ctx.last_buffer_handle = handle;
    auto index = ensureScalarIndex(valueArg(1, "0"));
    auto elem_offset = ensureScalarIndex(valueArg(2, "0"));
    auto byte_offset = "((" + index + ")*4 + (" + elem_offset + "))";
    return "(reinterpret_cast<device uint4&>(" + handle + "[" + byte_offset + "]))";
  }

  case DXOP_BufferStore:
  case DXOP_RawBufferStore:
  case DXOP_RawBufferStoreLegacy: {
    if (args.size() < 4) return "";
    auto handle = handleArg(0, "buf", "tex0");
    auto index = ensureScalarIndex(valueArg(1, "0"));
    auto elem_offset = ensureScalarIndex(valueArg(2, "0"));
    std::string base_offset = "((" + index + ")*4 + (" + elem_offset + "))";
    std::ostringstream store;
    uint32_t value_count = std::min<uint32_t>(4, (uint32_t)args.size() - 3);
    for (uint32_t i = 0; i < value_count; i++) {
      if (i)
        store << ";\n  ";
      store << "reinterpret_cast<device uint&>(" << handle << "[(" << base_offset
            << ") + " << (i * 4) << "]) = (uint)(" << valueArg(3 + i, "0")
            << ")";
    }
    return store.str();
  }

  case DXOP_RawBufferVectorStore: {
    if (args.size() < 4) return "";
    auto handle = handleArg(0, "buf", "tex0");
    auto index = ensureScalarIndex(valueArg(1, "0"));
    auto elem_offset = ensureScalarIndex(valueArg(2, "0"));
    auto value = valueArg(3, "uint4(0)");
    std::string base_offset = "((" + index + ")*4 + (" + elem_offset + "))";
    std::ostringstream store;
    for (uint32_t i = 0; i < 4; i++) {
      if (i)
        store << ";\n  ";
      store << "reinterpret_cast<device uint&>(" << handle << "[(" << base_offset
            << ") + " << (i * 4) << "]) = (uint)(" << value
            << componentSuffix(i) << ")";
    }
    return store.str();
  }

  case DXOP_TextureLoad: {
    if (args.size() < 3) return "float4(0)";
    auto handle = handleArg(0, "tex", "tex0");
    ctx.last_buffer_handle = handle;
    auto coord_x = ensureScalarIndex(valueArg(2, "0"));
    auto coord_y = ensureScalarIndex(valueArg(3, "0"));
    auto coord = "uint2(" + coord_x + ", " + coord_y + ")";
    return handle + ".read(" + coord + ")";
  }

  case DXOP_TextureStore:
  case DXOP_TextureStoreSample: {
    if (args.size() < 6) return "";
    auto handle = handleArg(0, "tex", "tex0");
    auto coord_x = ensureScalarIndex(valueArg(1, "0"));
    auto coord_y = ensureScalarIndex(valueArg(2, "0"));
    size_t value_base = intrinsic_id == DXOP_TextureStoreSample ? 5 : 4;
    auto value_x = valueArg(value_base + 0, "0.0");
    auto value_y = valueArg(value_base + 1, "0.0");
    auto value_z = valueArg(value_base + 2, "0.0");
    auto value_w = valueArg(value_base + 3, "0.0");
    if (intrinsic_id == DXOP_TextureStoreSample) {
      recordDiagnostic(ctx, "DXIL TextureStoreSample lowered without explicit sample index");
    }
    return handle + ".write(float4(" + value_x + ", " + value_y + ", " +
           value_z + ", " + value_w + "), uint2(" + coord_x + ", " +
           coord_y + "))";
  }

  case DXOP_TextureSample:
  case DXOP_TextureSampleBias:
  case DXOP_TextureSampleLevel:
  case DXOP_TextureSampleGrad: {
    if (args.size() < 4) return "float4(0)";
    auto handle = handleArg(0, "tex", "tex0");
    auto sampler = handleArg(1, "samp", "samp0");
    auto coord_x = ensureScalarIndex(valueArg(2, "0.0"));
    auto coord_y = ensureScalarIndex(valueArg(3, "0.0"));
    auto coord = "float2(" + coord_x + ", " + coord_y + ")";
    if (intrinsic_id == DXOP_TextureSampleGrad) {
      recordDiagnostic(ctx, "DXIL SampleGrad lowered without explicit gradients");
    } else if (intrinsic_id == DXOP_TextureSampleLevel) {
      recordDiagnostic(ctx, "DXIL SampleLevel lowered without explicit LOD");
    } else if (intrinsic_id == DXOP_TextureSampleBias) {
      recordDiagnostic(ctx, "DXIL SampleBias lowered without explicit bias");
    }
    return handle + ".sample(" + sampler + ", " + coord + ")";
  }

  case DXOP_TextureGather:
  case DXOP_TextureGatherCmp:
  case DXOP_TextureGatherRaw: {
    if (args.size() < 4) return "float4(0)";
    auto handle = handleArg(0, "tex", "tex0");
    auto sampler = handleArg(1, "samp", "samp0");
    auto coord_x = ensureScalarIndex(valueArg(2, "0.0"));
    auto coord_y = ensureScalarIndex(valueArg(3, "0.0"));
    uint32_t channel = args.size() > 8 ? literalArg(8, 0, "gather channel") : 0;
    if (intrinsic_id == DXOP_TextureGatherCmp) {
      recordDiagnostic(ctx, "DXIL TextureGatherCmp lowered without explicit compare");
    } else if (intrinsic_id == DXOP_TextureGatherRaw) {
      recordDiagnostic(ctx, "DXIL TextureGatherRaw lowered through typed gather");
    }
    return handle + ".gather(" + sampler + ", float2(" + coord_x + ", " +
           coord_y + "), component::" + componentName(channel) + ")";
  }

  case DXOP_TextureSampleCmp:
  case DXOP_TextureSampleCmpLevelZero:
  case DXOP_TextureSampleCmpLevel: {
    if (args.size() < 5) return "0.0";
    auto handle = handleArg(0, "tex", "tex0");
    auto sampler = handleArg(1, "samp", "samp0");
    auto coord_x = ensureScalarIndex(valueArg(2, "0.0"));
    auto coord_y = ensureScalarIndex(valueArg(3, "0.0"));
    auto compare = valueArg(4, "0.0");
    auto sample = handle + ".sample(" + sampler + ", float2(" + coord_x +
                  ", " + coord_y + ")).r";
    return "((" + sample + ") < (" + compare + ") ? 1.0 : 0.0)";
  }

  case DXOP_BufferUpdateCounter: {
    recordDiagnostic(ctx, "DXIL BufferUpdateCounter lowered to non-mutating counter fallback");
    return "0";
  }

  case DXOP_CheckAccessFullyMapped:
    return "true";

  case DXOP_GetDimensions: {
    if (args.empty()) return "uint4(0)";
    auto handle = handleArg(0, "tex", "tex0");
    return "uint4(" + handle + ".get_width(), " + handle +
           ".get_height(), 1, 1)";
  }

  case DXOP_DerivCoarseX:
  case DXOP_DerivFineX: {
    if (args.empty()) return "0.0";
    return "dfdx(" + valueArg(0, "0.0") + ")";
  }

  case DXOP_DerivCoarseY:
  case DXOP_DerivFineY: {
    if (args.empty()) return "0.0";
    return "dfdy(" + valueArg(0, "0.0") + ")";
  }

  case DXOP_CalcLOD: {
    if (args.size() < 4) return "0.0";
    auto handle = handleArg(0, "tex", "tex0");
    auto sampler = handleArg(1, "samp", "samp0");
    auto coord_x = ensureScalarIndex(valueArg(2, "0.0"));
    auto coord_y = ensureScalarIndex(valueArg(3, "0.0"));
    return handle + ".calculate_unclamped_lod(" + sampler + ", float2(" +
           coord_x + ", " + coord_y + "))";
  }

  case DXOP_AtomicBinOp: {
    if (args.size() < 4) return "0";
    auto handle = handleArg(1, "buf", "tex0");
    auto offset = ensureScalarIndex(valueArg(2, "0"));
    auto value = ensureScalarIndex(valueArg(3, "0"));
    return "atomic_fetch_add_explicit(reinterpret_cast<device atomic_uint*>(" +
           handle + " + (" + offset + ")), (uint)(" + value +
           "), memory_order_relaxed)";
  }

  case DXOP_AtomicCompareExchange: {
    if (args.size() < 2) return "0";
    auto handle = handleArg(0, "buf", "tex0");
    auto offset = ensureScalarIndex(valueArg(1, "0"));
    recordDiagnostic(ctx, "DXIL AtomicCompareExchange lowered to atomic load fallback");
    return "atomic_load_explicit(reinterpret_cast<device atomic_uint*>(" +
           handle + " + (" + offset + ")), memory_order_relaxed)";
  }

  case DXOP_Texture2DMSGetSamplePosition:
  case DXOP_RenderTargetGetSamplePosition:
  case DXOP_Texture2DMSGetSamplePositionLegacy:
  case DXOP_RenderTargetGetSamplePositionLegacy: {
    if (!args.empty()) {
      uint32_t component = literalArg(args.size() - 1, 0, "sample position component");
      return component == 0 ? "0.5" : "0.5";
    }
    return "0.5";
  }

  case DXOP_RenderTargetGetSampleCount:
    return "1";

  case DXOP_CycleCounterLegacy:
    ctx.unsupported_intrinsics++;
    recordDiagnostic(ctx,
                     "DXIL cycle-counter intrinsic is unsupported; rejecting shader");
    return "0";

  case DXOP_HitObjectOrSER:
    ctx.unsupported_intrinsics++;
    recordDiagnostic(
        ctx,
        "DXIL HitObject/SER intrinsic is unsupported; rejecting shader (opcode=%u)",
        explicit_opcode);
    return "0";

  case DXOP_Barrier: {
    return "threadgroup_barrier(mem_flags::mem_threadgroup)";
  }

  case DXOP_Unary: {
    if (args.size() < 2) return "0";
    uint32_t op = literalArg(0, 0xFFFFFFFFu, "unary opcode");
    auto x = valueArg(1, "0.0");
    switch (op) {
    case DXILOP_FAbs: return "abs(" + x + ")";
    case DXILOP_Saturate: return "clamp(" + x + ", 0.0, 1.0)";
    case DXILOP_IsNaN: return "isnan(" + x + ")";
    case DXILOP_IsInf: return "isinf(" + x + ")";
    case DXILOP_IsFinite: return "isfinite(" + x + ")";
    case DXILOP_Cos: return "cos(" + x + ")";
    case DXILOP_Sin: return "sin(" + x + ")";
    case DXILOP_Tan: return "tan(" + x + ")";
    case DXILOP_Acos: return "acos(" + x + ")";
    case DXILOP_Asin: return "asin(" + x + ")";
    case DXILOP_Atan: return "atan(" + x + ")";
    case DXILOP_Exp: return "exp2(" + x + ")";
    case DXILOP_Frc: return "fract(" + x + ")";
    case DXILOP_Log: return "log2(" + x + ")";
    case DXILOP_Sqrt: return "sqrt(" + x + ")";
    case DXILOP_Rsqrt: return "rsqrt(" + x + ")";
    case DXILOP_Round_ne: return "rint(" + x + ")";
    case DXILOP_Round_ni: return "floor(" + x + ")";
    case DXILOP_Round_pi: return "ceil(" + x + ")";
    case DXILOP_Round_z: return "trunc(" + x + ")";
    case DXILOP_Bfrev: return "reverse_bits(" + x + ")";
    case DXILOP_Countbits: return "popcount(" + x + ")";
    case DXILOP_FirstbitLo: return "ctz(" + x + ")";
    case DXILOP_FirstbitHi: return "clz(" + x + ")";
    case DXILOP_FirstbitSHi: return "((" + x + ") < 0 ? clz(~(" + x + ")) : clz(" + x + "))";
    default:
      ctx.unsupported_intrinsics++;
      recordDiagnostic(ctx, "DXIL unknown unary opcode: %u", op);
      return x;
    }
  }

  case DXOP_Binary: {
    if (args.size() < 3) return "0";
    uint32_t op = literalArg(0, 0xFFFFFFFFu, "binary opcode");
    auto a = valueArg(1, "0");
    auto b = valueArg(2, "0");
    switch (op) {
    case DXILOP_FMax:
    case DXILOP_IMax: return "max(" + a + ", " + b + ")";
    case DXILOP_FMin:
    case DXILOP_IMin: return "min(" + a + ", " + b + ")";
    case DXILOP_UMax: return "max((uint)(" + a + "), (uint)(" + b + "))";
    case DXILOP_UMin: return "min((uint)(" + a + "), (uint)(" + b + "))";
    case DXILOP_IMul: return "mul24(" + a + ", " + b + ")";
    case DXILOP_UMul: return "mul24((uint)(" + a + "), (uint)(" + b + "))";
    case DXILOP_UDiv: return "((uint)(" + a + ") / (uint)(" + b + "))";
    case DXILOP_UAddc: return "((" + a + ") + (" + b + "))";
    case DXILOP_USubb: return "((" + a + ") - (" + b + "))";
    default:
      ctx.unsupported_intrinsics++;
      recordDiagnostic(ctx, "DXIL unknown binary opcode: %u", op);
      return a;
    }
  }

  case DXOP_Tertiary: {
    if (args.size() < 4) return "0";
    uint32_t op = literalArg(0, 0xFFFFFFFFu, "tertiary opcode");
    auto a = valueArg(1, "0");
    auto b = valueArg(2, "0");
    auto c = valueArg(3, "0");
    switch (op) {
    case DXILOP_FMad:
    case DXILOP_Fma: return "fma(" + a + ", " + b + ", " + c + ")";
    case DXILOP_IMad:
    case DXILOP_UMad: return "((" + a + ") * (" + b + ") + (" + c + "))";
    case DXILOP_Msad: return "((" + a + ") & (" + b + ")) ^ (" + c + ")";
    case DXILOP_Ibfe: return "extract_bits(" + a + ", " + b + ", " + c + ")";
    case DXILOP_Ubfe: return "extract_bits((uint)(" + a + "), (uint)(" + b + "), (uint)(" + c + "))";
    case DXILOP_Bfi: return "insert_bits((uint)(" + b + "), (uint)(" + a + "), (uint)(" + c + "))";
    default:
      ctx.unsupported_intrinsics++;
      recordDiagnostic(ctx, "DXIL unknown tertiary opcode: %u", op);
      return a;
    }
  }

  case DXOP_Dot2: {
    if (args.size() < 4) return "0.0";
    auto ax = valueArg(0, "0.0");
    auto ay = valueArg(1, "0.0");
    auto bx = valueArg(2, "0.0");
    auto by = valueArg(3, "0.0");
    return "((" + ax + ")*(" + bx + ") + (" + ay + ")*(" + by + "))";
  }

  case DXOP_Dot3: {
    if (args.size() < 6) return "0.0";
    auto ax = valueArg(0, "0.0");
    auto ay = valueArg(1, "0.0");
    auto az = valueArg(2, "0.0");
    auto bx = valueArg(3, "0.0");
    auto by = valueArg(4, "0.0");
    auto bz = valueArg(5, "0.0");
    return "((" + ax + ")*(" + bx + ") + (" + ay + ")*(" + by + ") + (" + az + ")*(" + bz + "))";
  }

  case DXOP_Dot4: {
    if (args.size() < 8) return "0.0";
    auto ax = valueArg(0, "0.0");
    auto ay = valueArg(1, "0.0");
    auto az = valueArg(2, "0.0");
    auto aw = valueArg(3, "0.0");
    auto bx = valueArg(4, "0.0");
    auto by = valueArg(5, "0.0");
    auto bz = valueArg(6, "0.0");
    auto bw = valueArg(7, "0.0");
    return "((" + ax + ")*(" + bx + ") + (" + ay + ")*(" + by + ") + (" + az + ")*(" + bz + ") + (" + aw + ")*(" + bw + "))";
  }

  case DXOP_LoadInput: {
    if (args.size() < 3) return "0.0";
    uint32_t input_id = literalArg(0, 0, "input id");
    uint32_t component = literalArg(2, 0, "input component");
    if (ctx.shader.kind == DxilShaderKind::Pixel) {
      if (ctx.shader.shading_rate_input_register >= 0 &&
          static_cast<int32_t>(input_id) ==
              ctx.shader.shading_rate_input_register)
        return "uint(" + varyingField("in", input_id) + ".x)";
      return varyingField("in", input_id) + componentSuffix(component);
    }
    if (ctx.shader.kind == DxilShaderKind::Vertex) {
      return vertexInputField("vin", input_id) + componentSuffix(component);
    }
    recordDiagnostic(ctx, "DXIL LoadInput fallback: shader_kind=%u input_id=%u component=%u",
                     (uint32_t)ctx.shader.kind, input_id, component);
    return "0.0";
  }

  case DXOP_StoreOutput: {
    if (args.size() < 4) return "";
    uint32_t output_id = literalArg(0, 0, "output id");
    uint32_t component = literalArg(2, 0, "output component");
    auto val = valueArg(3, "float4(0)");

    if (ctx.shader.kind == DxilShaderKind::Vertex) {
      if (ctx.shader.shading_rate_output_register >= 0 &&
          static_cast<int32_t>(output_id) ==
              ctx.shader.shading_rate_output_register)
        return varyingField("out", output_id) + ".x = float(" + val + ")";
      return varyingField("out", output_id) + componentSuffix(component) + " = " + val;
    }
    if (ctx.shader.kind == DxilShaderKind::Pixel) {
      if (output_id > 0) {
        recordDiagnostic(ctx, "DXIL StoreOutput MRT fallback: output_id=%u component=%u",
                         output_id, component);
      }
      return std::string("result") + componentSuffix(component) + " = " + val;
    }
    recordDiagnostic(ctx, "DXIL StoreOutput fallback: shader_kind=%u output_id=%u component=%u",
                     (uint32_t)ctx.shader.kind, output_id, component);
    return "";
  }

  case DXOP_LegacyF16ToF32: {
    if (args.size() < 2) return "0.0f";
    return "as_type<float>(half(" + valueArg(1, "0") + "))";
  }

  case DXOP_LegacyF32ToF16: {
    if (args.size() < 2) return "0";
    return "as_type<uint>(half(" + valueArg(1, "0.0") + "))";
  }

  case DXOP_WaveReadLaneFirst: {
    if (args.size() < 2) return "0";
    return "simd_broadcast_first(" + valueArg(1, "0") + ")";
  }

  case DXOP_WaveReadLaneAt: {
    if (args.size() < 3) return "0";
    return "simd_broadcast(" + valueArg(1, "0") + ", (uint)(" + valueArg(2, "0") + "))";
  }

  case DXOP_WaveIsFirstLane:
    return "simd_is_first()";

  case DXOP_WaveGetLaneIndex:
    return "simd_lane_id()";

  case DXOP_WaveGetLaneCount:
    return "simd_lane_count()";

  case DXOP_WaveAnyTrue: {
    if (args.size() < 2) return "0";
    return "simd_any(" + valueArg(1, "0") + ") ? 1 : 0";
  }

  case DXOP_WaveAllTrue: {
    if (args.size() < 2) return "0";
    return "simd_all(" + valueArg(1, "0") + ") ? 1 : 0";
  }

  case DXOP_QuadReadLaneAt: {
    if (args.size() < 3) return "0";
    return "quad_broadcast(" + valueArg(1, "0") + ", (uint)(" + valueArg(2, "0") + "))";
  }

  default:
    ctx.unsupported_intrinsics++;
    recordDiagnostic(ctx, "DXIL unknown intrinsic: %u", intrinsic_id);
    break;
  }

  return "0 /* unknown dx intrinsic " + std::to_string(intrinsic_id) + " */";
}

void DXILToMSL::emitInstruction(EmitContext &ctx, const LLVMInstruction &inst, uint32_t &value_counter) {
  auto &os = ctx.os;
  std::string result = emitValue(value_counter);

  auto getValue = [&](uint32_t idx) -> std::string {
    if (idx < ctx.value_table.size() && !ctx.value_table[idx].empty()) {
      const auto &v = ctx.value_table[idx];
      if (v.find('.') != std::string::npos && !startsWith(v, "dx."))
        return v;
      if (startsWith(v, "agg(")) {
        auto parts = parseAggregateLiteral(v);
        uint32_t tid = idx < ctx.value_type_ids.size() ? ctx.value_type_ids[idx] : 0;
        if (tid < ctx.mod.types.size()) {
          auto &ty = ctx.mod.types[tid];
          if (ty.kind == LLVMType::Struct && !ty.subtypes.empty() && !parts.empty()) {
            std::string args;
            for (size_t p = 0; p < parts.size(); p++) {
              if (p) args += ",";
              args += parts[p];
            }
            if (ty.subtypes[0].kind == LLVMType::Integer) {
              return "int" + std::to_string(parts.size()) + "(" + args + ")";
            }
            return "int" + std::to_string(parts.size()) + "(" + args + ")";
          }
        }
        std::string args;
        for (size_t p = 0; p < parts.size(); p++) {
          if (p) args += ",";
          args += parts[p];
        }
        return "int" + std::to_string(parts.size()) + "(" + args + ")";
      }
      return v;
    }
    for (auto &c : ctx.mod.constants) {
      if (c.id == idx) {
        if (!c.constant_data.empty()) return c.constant_data;
        break;
      }
    }
    if (ctx.current_fn) {
      for (auto &c : ctx.current_fn->constants) {
        if (c.id == idx) {
          if (!c.constant_data.empty()) return c.constant_data;
          break;
        }
      }
    }
    return emitValue(idx);
  };

  auto ensureValueTable = [&](uint32_t needed) {
    if (ctx.value_table.size() <= needed)
      ctx.value_table.resize(needed + 1);
    if (ctx.value_type_ids.size() <= needed)
      ctx.value_type_ids.resize(needed + 1, 0);
  };

  switch (inst.opcode) {
  case LLVMInstruction::Ret:
    if (ctx.shader.kind == DxilShaderKind::Vertex) {
      os << "  return out;\n";
    } else if (ctx.shader.kind == DxilShaderKind::Pixel) {
      os << "  return result;\n";
    } else {
      os << "  return;\n";
    }
    break;

  case LLVMInstruction::Call: {
    if (inst.operands.empty())
      break;
    uint32_t callee = inst.operands[0];

    std::vector<uint32_t> call_args;
    for (size_t i = 2; i < inst.operands.size(); i++)
      call_args.push_back(inst.operands[i]);

    std::string callee_name;
    auto decl_it = ctx.function_decls.find(callee);
    if (decl_it != ctx.function_decls.end()) {
      callee_name = decl_it->second;
    } else if (callee < ctx.value_table.size()) {
      callee_name = ctx.value_table[callee];
    }
    uint32_t intrinsic_id = intrinsicIdFromCalleeName(callee_name);

    if (intrinsic_id != 0 && call_args.empty()) {
      ctx.unsupported_intrinsics++;
      os << "  // dx.op." << callee_name.substr(6) << " with no arguments\n";
      ensureValueTable(value_counter);
      ctx.value_table[value_counter] = result;
      ctx.value_type_ids[value_counter] = inst.type_id;
    } else if (intrinsic_id != 0) {
      std::vector<uint32_t> fn_args;
      if (intrinsic_id == DXOP_Unary || intrinsic_id == DXOP_Binary || intrinsic_id == DXOP_Tertiary) {
        fn_args = call_args;
      } else {
        fn_args.assign(call_args.begin() + 1, call_args.end());
      }

      uint32_t explicit_opcode = 0;
      if (!call_args.empty())
        parseUnsignedLiteral(getValue(call_args[0]), explicit_opcode);
      std::string translated =
          translateDXIntrinsic(ctx, intrinsic_id, fn_args, explicit_opcode);

      if (inst.type_id == 0) {
        ensureValueTable(value_counter);
        if (!translated.empty()) {
          os << "  " << translated << ";\n";
          ctx.value_table[value_counter] = translated;
        }
        ctx.value_type_ids[value_counter] = inst.type_id;
      } else if (translated.find('=') == std::string::npos) {
        ensureValueTable(value_counter);
        if (!translated.empty() && translated[0] != ' ') {
          os << "  auto " << result << " = " << translated << ";\n";
          ctx.value_table[value_counter] = result;
          ctx.value_type_ids[value_counter] = inst.type_id;
          if (!ctx.last_buffer_handle.empty()) {
            ctx.buffer_origin[value_counter] = ctx.last_buffer_handle;
            ctx.last_buffer_handle.clear();
          }
        } else if (!translated.empty()) {
          os << "  " << translated << ";\n";
          ctx.value_table[value_counter] = translated;
        }
      } else {
        os << "  " << translated << ";\n";
        ensureValueTable(value_counter);
        ctx.value_table[value_counter] = translated;
      }
    } else {
      ensureValueTable(value_counter);
      if (inst.type_id != 0) {
        os << "  auto " << result << " = 0; // call " << (callee_name.empty() ? getValue(callee) : callee_name) << "(";
      } else {
        os << "  // call " << (callee_name.empty() ? getValue(callee) : callee_name) << "(";
      }
      for (size_t i = 0; i < call_args.size(); i++) {
        if (i) os << ", ";
        os << getValue(call_args[i]);
      }
      os << ")\n";
      ctx.value_table[value_counter] = result;
      ctx.value_type_ids[value_counter] = inst.type_id;
    }
    value_counter++;
    break;
  }

  case LLVMInstruction::Add:
  case LLVMInstruction::Sub:
  case LLVMInstruction::Mul:
  case LLVMInstruction::UDiv:
  case LLVMInstruction::SDiv:
  case LLVMInstruction::URem:
  case LLVMInstruction::SRem:
  case LLVMInstruction::And:
  case LLVMInstruction::Or:
  case LLVMInstruction::Xor:
  case LLVMInstruction::Shl:
  case LLVMInstruction::LShr:
  case LLVMInstruction::AShr: {
    ensureValueTable(value_counter);
    const char *op_str = "+";
    switch (inst.opcode) {
    case LLVMInstruction::Add: op_str = "+"; break;
    case LLVMInstruction::Sub: op_str = "-"; break;
    case LLVMInstruction::Mul: op_str = "*"; break;
    case LLVMInstruction::UDiv: op_str = "/"; break;
    case LLVMInstruction::SDiv: op_str = "/"; break;
    case LLVMInstruction::URem: op_str = "%"; break;
    case LLVMInstruction::SRem: op_str = "%"; break;
    case LLVMInstruction::And: op_str = "&"; break;
    case LLVMInstruction::Or: op_str = "|"; break;
    case LLVMInstruction::Xor: op_str = "^"; break;
    case LLVMInstruction::Shl: op_str = "<<"; break;
    case LLVMInstruction::LShr: op_str = ">>"; break;
    case LLVMInstruction::AShr: op_str = ">>"; break;
    default: break;
    }
    os << "  auto " << result << " = " << getValue(inst.operands[0]) << " " << op_str << " " << getValue(inst.operands[1]) << ";\n";
    ctx.value_table[value_counter] = result;
    if (inst.operands.size() >= 1 && inst.operands[0] < ctx.value_type_ids.size())
      ctx.value_type_ids[value_counter] = ctx.value_type_ids[inst.operands[0]];
    value_counter++;
    break;
  }

  case LLVMInstruction::ExtractValue: {
    ensureValueTable(value_counter);
    if (inst.operands.size() >= 2) {
      auto agg = getValue(inst.operands[0]);
      auto idx = inst.operands[1];

      bool agg_is_struct = false;
      uint32_t agg_type_id = (inst.operands[0] < ctx.value_type_ids.size())
                                 ? ctx.value_type_ids[inst.operands[0]]
                                 : 0;
      if (agg_type_id > 0 && agg_type_id < ctx.mod.types.size() &&
          ctx.mod.types[agg_type_id].kind == LLVMType::Struct) {
        agg_is_struct = true;
      }

      uint32_t result_type_id = 0;
      if (agg_is_struct) {
        if (idx == 0) {
          os << "  auto " << result << " = " << agg
             << "; // extractvalue struct field 0\n";
        } else {
          std::string default_val = "0";
          if (agg_type_id < ctx.mod.types.size()) {
            auto &st = ctx.mod.types[agg_type_id];
            if (idx < st.type_refs.size()) {
              uint32_t field_type_id = st.type_refs[idx];
              if (field_type_id < ctx.mod.types.size()) {
                auto &fty = ctx.mod.types[field_type_id];
                if (fty.kind == LLVMType::Float)
                  default_val = "0.0f";
                else if (fty.kind == LLVMType::Double)
                  default_val = "0.0";
              }
            }
          }
          os << "  auto " << result << " = " << default_val
             << "; // extractvalue struct field " << idx << " (default)\n";
        }
        if (agg_type_id < ctx.mod.types.size() &&
            idx < ctx.mod.types[agg_type_id].type_refs.size())
          result_type_id = ctx.mod.types[agg_type_id].type_refs[idx];
      } else if (idx < 4) {
        os << "  auto " << result << " = (" << agg << ")"
           << componentSuffix(idx) << "; // extractvalue\n";
      } else {
        os << "  auto " << result << " = (" << agg
           << "); // extractvalue idx=" << idx << "\n";
      }
      ctx.value_table[value_counter] = result;
      ctx.value_type_ids[value_counter] = result_type_id;
    } else {
      os << "  auto " << result << " = 0; // extractvalue short\n";
      ctx.value_table[value_counter] = result;
    }
    value_counter++;
    break;
  }

  case LLVMInstruction::InsertValue: {
    ensureValueTable(value_counter);
    os << "  auto " << result << " = "
       << (inst.operands.size() >= 1 ? getValue(inst.operands[0]) : "float4(0)")
       << "; // insertvalue\n";

    bool agg_is_struct = false;
    if (inst.operands.size() >= 1 && inst.operands[0] < ctx.value_type_ids.size()) {
      uint32_t agg_type_id = ctx.value_type_ids[inst.operands[0]];
      if (agg_type_id > 0 && agg_type_id < ctx.mod.types.size() &&
          ctx.mod.types[agg_type_id].kind == LLVMType::Struct) {
        agg_is_struct = true;
      }
    }

    if (inst.operands.size() >= 3) {
      if (agg_is_struct) {
        os << "  // insertvalue struct field " << inst.operands[2]
           << " — skipped (struct field set not yet supported)\n";
      } else if (inst.operands[2] < 4) {
        os << "  " << result << componentSuffix(inst.operands[2])
           << " = " << getValue(inst.operands[1]) << ";\n";
      }
    }

    ctx.value_table[value_counter] = result;
    if (inst.operands.size() >= 1 && inst.operands[0] < ctx.value_type_ids.size())
      ctx.value_type_ids[value_counter] = ctx.value_type_ids[inst.operands[0]];
    value_counter++;
    break;
  }

  case LLVMInstruction::ExtractElement: {
    ensureValueTable(value_counter);
    if (inst.operands.size() >= 2) {
      auto idx = getValue(inst.operands[1]);
      os << "  auto " << result << " = " << getValue(inst.operands[0])
         << componentAccessor(idx) << ";\n";
      ctx.value_table[value_counter] = result;
    }
    value_counter++;
    break;
  }

  case LLVMInstruction::InsertElement: {
    ensureValueTable(value_counter);
    if (inst.operands.size() >= 3) {
      auto vec = getValue(inst.operands[0]);
      auto elem = getValue(inst.operands[1]);
      auto idx = getValue(inst.operands[2]);
      os << "  auto " << result << " = " << vec << ";\n";
      os << "  " << result << componentAccessor(idx) << " = " << elem << ";\n";
    } else {
      os << "  auto " << result << " = "
         << (inst.operands.size() >= 1 ? getValue(inst.operands[0]) : "float4(0)")
         << "; // insertelement fallback\n";
    }
    ctx.value_table[value_counter] = result;
    value_counter++;
    break;
  }

  case LLVMInstruction::ShuffleVector: {
    ensureValueTable(value_counter);
    auto lhs = inst.operands.size() >= 1 ? getValue(inst.operands[0]) : "float4(0)";
    auto rhs = inst.operands.size() >= 2 ? getValue(inst.operands[1]) : "float4(0)";
    auto mask = inst.operands.size() >= 3 ? getValue(inst.operands[2]) : "";
    auto mask_values = parseAggregateLiteral(mask);
    if (mask_values.empty()) {
      uint32_t single_index = 0;
      if (parseUnsignedLiteral(mask, single_index))
        mask_values.push_back(mask);
    }

    if (!mask_values.empty()) {
      std::vector<std::string> components;
      for (auto &mask_value : mask_values) {
        uint32_t index = 0;
        if (!parseUnsignedLiteral(mask_value, index) || index == 0xFFFFFFFFu) {
          components.push_back("0.0f");
        } else if (index < 4) {
          components.push_back(vectorComponent(lhs, index));
        } else {
          components.push_back(vectorComponent(rhs, index - 4));
        }
      }

      if (components.size() == 1) {
        os << "  auto " << result << " = " << components[0]
           << "; // shufflevector scalar\n";
      } else {
        std::string type_name = "float" + std::to_string(components.size());
        if (inst.type_id < ctx.mod.types.size() &&
            ctx.mod.types[inst.type_id].kind == LLVMType::Vector) {
          type_name = getTypeName(ctx.mod.types[inst.type_id], ctx.mod);
        }
        os << "  auto " << result << " = " << type_name << "(";
        for (size_t i = 0; i < components.size(); i++) {
          if (i) os << ", ";
          os << components[i];
        }
        os << "); // shufflevector\n";
      }
    } else {
      recordDiagnostic(ctx, "DXIL shufflevector fallback: mask=%s", mask.c_str());
      os << "  auto " << result << " = " << lhs << "; // shufflevector fallback\n";
    }
    ctx.value_table[value_counter] = result;
    value_counter++;
    break;
  }

  case LLVMInstruction::Unreachable:
    os << "  // unreachable\n";
    break;

  case LLVMInstruction::FNeg: {
    ensureValueTable(value_counter);
    if (inst.operands.size() >= 1) {
      os << "  auto " << result << " = -(" << getValue(inst.operands[0]) << ");\n";
      ctx.value_table[value_counter] = result;
    }
    value_counter++;
    break;
  }

  case LLVMInstruction::FAdd:
  case LLVMInstruction::FSub:
  case LLVMInstruction::FMul:
  case LLVMInstruction::FDiv:
  case LLVMInstruction::FRem: {
    ensureValueTable(value_counter);
    const char *fop_str = "+";
    switch (inst.opcode) {
    case LLVMInstruction::FAdd: fop_str = "+"; break;
    case LLVMInstruction::FSub: fop_str = "-"; break;
    case LLVMInstruction::FMul: fop_str = "*"; break;
    case LLVMInstruction::FDiv: fop_str = "/"; break;
    case LLVMInstruction::FRem: fop_str = "%"; break;
    default: break;
    }
    if (inst.operands.size() >= 2) {
      os << "  auto " << result << " = " << getValue(inst.operands[0]) << " " << fop_str << " " << getValue(inst.operands[1]) << ";\n";
      ctx.value_table[value_counter] = result;
      if (inst.operands[0] < ctx.value_type_ids.size())
        ctx.value_type_ids[value_counter] = ctx.value_type_ids[inst.operands[0]];
    }
    value_counter++;
    break;
  }

  case LLVMInstruction::BitCast: {
    ensureValueTable(value_counter);
    if (inst.operands.size() >= 1) {
      std::string val = getValue(inst.operands[0]);
      if (inst.type_id > 0 && inst.type_id < ctx.mod.types.size() &&
          inst.operands[0] < ctx.value_type_ids.size()) {
        auto &dst_type = ctx.mod.types[inst.type_id];
        auto src_type_id = ctx.value_type_ids[inst.operands[0]];
        if (src_type_id > 0 && src_type_id < ctx.mod.types.size()) {
          auto &src_type = ctx.mod.types[src_type_id];
          std::string dst_name = getTypeName(dst_type, ctx.mod);
          std::string src_name = getTypeName(src_type, ctx.mod);
          if (dst_name != src_name && !dst_name.empty() && !src_name.empty()) {
            os << "  auto " << result << " = reinterpret_cast<" << dst_name << ">(" << val << "); // bitcast\n";
          } else {
            os << "  auto " << result << " = " << val << "; // bitcast same type\n";
          }
        } else {
          os << "  auto " << result << " = " << val << "; // bitcast\n";
        }
      } else {
        os << "  auto " << result << " = " << val << "; // bitcast\n";
      }
      ctx.value_table[value_counter] = result;
      if (inst.operands[0] < ctx.value_type_ids.size())
        ctx.value_type_ids[value_counter] = ctx.value_type_ids[inst.operands[0]];
    }
    value_counter++;
    break;
  }

  case LLVMInstruction::FCmp: {
    ensureValueTable(value_counter);
    if (inst.operands.size() >= 3) {
      uint32_t pred = inst.operands[0];
      auto lhs = getValue(inst.operands[1]);
      auto rhs = getValue(inst.operands[2]);
      const char *cmp_str = "false";
      switch (pred) {
      case 0: cmp_str = "false"; break; // FALSE
      case 1: cmp_str = "=="; break;    // OEQ
      case 2: cmp_str = ">"; break;     // OGT
      case 3: cmp_str = ">="; break;    // OGE
      case 4: cmp_str = "<"; break;     // OLT
      case 5: cmp_str = "<="; break;    // OLE
      case 6: cmp_str = "!="; break;    // ONE
      case 7: cmp_str = "true"; break;  // ORD (both not NaN)
      case 8: cmp_str = "=="; break;    // UEQ
      case 9: cmp_str = ">="; break;    // UGT (actually > with unordered)
      case 10: cmp_str = ">="; break;   // UGE
      case 11: cmp_str = "<"; break;    // ULT (actually < with unordered)
      case 12: cmp_str = "<="; break;   // ULE
      case 13: cmp_str = "!="; break;   // UNE
      case 14: cmp_str = "true"; break; // UNO (either NaN)
      case 15: cmp_str = "true"; break; // TRUE
      }
      if (pred == 0 || pred > 15) {
        os << "  auto " << result << " = false; // fcmp false pred=" << pred << "\n";
      } else if (pred == 15) {
        os << "  auto " << result << " = true; // fcmp true\n";
      } else if (pred == 7) {
        os << "  bool " << result << " = (!isnan(" << lhs << ") && !isnan(" << rhs << ")); // fcmp ord\n";
      } else if (pred == 14) {
        os << "  bool " << result << " = (isnan(" << lhs << ") || isnan(" << rhs << ")); // fcmp uno\n";
      } else {
        os << "  auto " << result << " = " << lhs << " " << cmp_str << " " << rhs << "; // fcmp " << pred << "\n";
      }
      ctx.value_table[value_counter] = result;
      ctx.value_type_ids[value_counter] = inst.type_id;
    }
    value_counter++;
    break;
  }

  case LLVMInstruction::Alloca: {
    std::string storage_class = "thread";
    if (inst.type_id > 0 && inst.type_id < ctx.mod.types.size()) {
      auto &ptr_type = ctx.mod.types[inst.type_id];
      if (ptr_type.kind == LLVMType::Pointer && ptr_type.address_space == 3)
        storage_class = "threadgroup";
    }
    std::string alloca_name = "alloca_" + std::to_string(value_counter);
    os << "  " << storage_class << " char " << alloca_name << "[256];\n";
    ensureValueTable(value_counter);
    ctx.value_table[value_counter] = "(" + storage_class + " char*)&" + alloca_name;
    if (ctx.value_type_ids.size() <= value_counter)
      ctx.value_type_ids.resize(value_counter + 1, 0);
    ctx.value_type_ids[value_counter] = inst.type_id;
    value_counter++;
    break;
  }

  case LLVMInstruction::Load: {
    if (inst.operands.size() < 1) {
      ensureValueTable(value_counter);
      ctx.value_table[value_counter] = "0";
      value_counter++;
      break;
    }
    auto ptr_expr = getValue(inst.operands[0]);
    std::string loaded_type = "int";
    if (inst.type_id > 0 && inst.type_id < ctx.mod.types.size()) {
      loaded_type = getTypeName(ctx.mod.types[inst.type_id], ctx.mod);
    }
    std::string load_expr = "*(" + loaded_type + "*)(" + ptr_expr + ")";
    ensureValueTable(value_counter);
    ctx.value_table[value_counter] = load_expr;
    if (ctx.value_type_ids.size() <= value_counter)
      ctx.value_type_ids.resize(value_counter + 1, 0);
    ctx.value_type_ids[value_counter] = inst.type_id;
    recordDiagnostic(ctx, "DXIL generic load: ptr=%s type=%s", emitValue(inst.operands[0]).c_str(), loaded_type.c_str());
    value_counter++;
    break;
  }

  case LLVMInstruction::Store: {
    if (inst.operands.size() < 2) {
      break;
    }
    auto ptr_expr = getValue(inst.operands[0]);
    auto val_expr = getValue(inst.operands[1]);
    os << "  *(" << ptr_expr << ") = " << val_expr << "; // store\n";
    break;
  }

  case LLVMInstruction::GetElementPtr: {
    if (inst.operands.empty()) {
      ensureValueTable(value_counter);
      ctx.value_table[value_counter] = "0";
      value_counter++;
      break;
    }
    auto base_expr = getValue(inst.operands[0]);
    std::string gep_expr = base_expr;
    for (size_t i = 1; i < inst.operands.size(); i++) {
      auto idx_expr = getValue(inst.operands[i]);
      if (isZeroLiteral(idx_expr))
        continue;
      gep_expr = gep_expr + " + " + idx_expr;
    }
    ensureValueTable(value_counter);
    ctx.value_table[value_counter] = gep_expr;
    if (ctx.value_type_ids.size() <= value_counter)
      ctx.value_type_ids.resize(value_counter + 1, 0);
    ctx.value_type_ids[value_counter] = inst.type_id;
    recordDiagnostic(ctx, "DXIL gep: base=%s result=%s indices=%zu", emitValue(inst.operands[0]).c_str(), emitValue(value_counter).c_str(), inst.operands.size() - 1);
    value_counter++;
    break;
  }

  case LLVMInstruction::Invoke: {
    os << "  // invoke\n";
    break;
  }

  default:
    ctx.unsupported_opcodes++;
    recordDiagnostic(ctx, "DXIL unhandled opcode: %d type=%u operands=%zu", (int)inst.opcode,
                     inst.type_id, inst.operands.size());
    os << "  // unhandled opcode " << (int)inst.opcode << "\n";
    ensureValueTable(value_counter);
    ctx.value_table[value_counter] = result;
    value_counter++;
    break;
  }
}

std::optional<MSLShader> DXILToMSL::convert(const LLVMModule &module,
                                              const DxilParsedShader &shader) {
  DXTRACE("DXILToMSL::convert: kind=%u sm=%u.%u functions=%zu types=%zu",
          (uint32_t)shader.kind, shader.shader_model.major, shader.shader_model.minor,
          module.functions.size(), module.types.size());

  std::ostringstream os;
  EmitContext ctx{os, module, shader, {}, {}, {}, {}, {}, {}, {}, {}, 0, 0, 0, 0, nullptr, false, false,
                  false, false};

  if (module.functions.empty()) {
    DXTRACE("DXILToMSL: refusing to emit default shader for module with no functions");
    return std::nullopt;
  }

  const LLVMFunction *entry_fn = nullptr;
  for (auto &fn : module.functions) {
    if (!fn.blocks.empty() && !shader.entry_point.empty() &&
        fn.name == shader.entry_point) {
      entry_fn = &fn;
      break;
    }
  }
  if (!entry_fn) {
    for (auto it = module.functions.rbegin(); it != module.functions.rend(); ++it) {
      if (!it->blocks.empty()) {
        entry_fn = &*it;
        break;
      }
    }
  }
  if (!entry_fn) {
    DXTRACE("DXILToMSL: refusing to emit shader because no parsed function has blocks");
    return std::nullopt;
  }

  auto &fn = *entry_fn;
  ctx.current_fn = entry_fn;

  emitFunctionPrologue(ctx);

  // Emit global variable declarations (groupshared/threadgroup)
  for (auto &gv : module.globals) {
    if (gv.address_space == 3) {
      std::string gv_name = gv.name.empty() ? "gvar_" + std::to_string(gv.value_id) : escapeName(gv.name);
      uint32_t array_size = 256;
      if (gv.type_id < module.types.size()) {
        auto &ptr_type = module.types[gv.type_id];
        if (ptr_type.kind == LLVMType::Pointer && !ptr_type.type_refs.empty()) {
          uint32_t pointee_type_id = ptr_type.type_refs[0];
          if (pointee_type_id < module.types.size())
            array_size = getTypeSize(module.types[pointee_type_id], module);
          if (array_size == 0) array_size = 256;
        }
      }
      os << "  threadgroup char " << gv_name << "[" << array_size << "]; // groupshared\n";
      if (gv.value_id < 256 || ctx.value_table.size() > gv.value_id) {
        if (ctx.value_table.size() <= gv.value_id)
          ctx.value_table.resize(gv.value_id + 1);
        ctx.value_table[gv.value_id] = "(threadgroup char*)&" + gv_name;
        if (ctx.value_type_ids.size() <= gv.value_id)
          ctx.value_type_ids.resize(gv.value_id + 1, 0);
        ctx.value_type_ids[gv.value_id] = gv.type_id;
      }
      DXTRACE("DXIL groupshared global: value=%u name=%s size=%u", gv.value_id, gv_name.c_str(), array_size);
    }
  }

  ctx.value_table.resize(256);
  ctx.value_type_ids.resize(256, 0);

  auto loadConstants = [&](const std::vector<LLVMValue> &consts) {
    for (size_t i = 0; i < consts.size(); i++) {
      uint32_t val_idx = consts[i].id;
      if (ctx.value_table.size() <= val_idx)
        ctx.value_table.resize(val_idx + 1);
      if (ctx.value_type_ids.size() <= val_idx)
        ctx.value_type_ids.resize(val_idx + 1, 0);
      if (val_idx < ctx.value_table.size()) {
        std::string cdata = consts[i].constant_data;
        if (cdata.substr(0, 4) == "agg(") {
          uint32_t tid = consts[i].type_id;
          std::string args = cdata.substr(4, cdata.size() - 5);
          if (tid < module.types.size()) {
            auto &ty = module.types[tid];
            if (ty.kind == LLVMType::Vector && !ty.subtypes.empty()) {
              std::string elem = getTypeName(ty.subtypes[0], module);
              uint32_t count = ty.subtypes[0].bit_width > 0 ? ty.bit_width / ty.subtypes[0].bit_width : 4;
              ctx.value_table[val_idx] = elem + std::to_string(count) + "(" + args + ")";
            } else {
              ctx.value_table[val_idx] = cdata;
            }
          } else {
            ctx.value_table[val_idx] = cdata;
          }
        } else {
          ctx.value_table[val_idx] = cdata.empty() ? "const_" + std::to_string(i) : cdata;
        }
        ctx.value_type_ids[val_idx] = consts[i].type_id;
      }
    }
  };

  loadConstants(module.constants);
  loadConstants(fn.constants);

  for (auto &decl_fn : module.functions) {
    if (!decl_fn.is_declaration || decl_fn.name.empty())
      continue;
    if (ctx.value_table.size() <= decl_fn.value_id)
      ctx.value_table.resize(decl_fn.value_id + 1);
    if (ctx.value_type_ids.size() <= decl_fn.value_id)
      ctx.value_type_ids.resize(decl_fn.value_id + 1, 0);
    ctx.value_table[decl_fn.value_id] = decl_fn.name;
    ctx.value_type_ids[decl_fn.value_id] = decl_fn.type_id;
    ctx.function_decls[decl_fn.value_id] = decl_fn.name;
    DXTRACE("DXIL function declaration: value=%u name=%s", decl_fn.value_id, decl_fn.name.c_str());
  }

  DXTRACE("DXILToMSL: entry function has %zu blocks", fn.blocks.size());

  // Build value ID -> block index map
  std::map<uint32_t, uint32_t> block_value_to_index;
  for (size_t i = 0; i < fn.block_value_ids.size(); i++) {
    block_value_to_index[fn.block_value_ids[i]] = (uint32_t)i;
  }

  // Build successor map: block_index -> set of successor block indices
  std::map<uint32_t, std::set<uint32_t>> successors;
  // Also collect terminator info per block
  struct TerminatorInfo {
    enum Kind { None, Br, Switch, Ret, Unreachable } kind = None;
    std::vector<uint32_t> operands;
  };
  std::vector<TerminatorInfo> terminators(fn.blocks.size());

  for (size_t bi = 0; bi < fn.blocks.size(); bi++) {
    auto &block = fn.blocks[bi];
    if (block.instructions.empty()) continue;
    auto &last = block.instructions.back();
    if (last.opcode == LLVMInstruction::Br) {
      terminators[bi].kind = TerminatorInfo::Br;
      terminators[bi].operands = last.operands;
      if (last.operands.size() == 1) {
        successors[(uint32_t)bi].insert(last.operands[0]);
      } else if (last.operands.size() >= 3) {
        successors[(uint32_t)bi].insert(last.operands[1]);
        successors[(uint32_t)bi].insert(last.operands[2]);
      }
    } else if (last.opcode == LLVMInstruction::Switch) {
      terminators[bi].kind = TerminatorInfo::Switch;
      terminators[bi].operands = last.operands;
      if (last.operands.size() >= 2) {
        successors[(uint32_t)bi].insert(last.operands[1]);
        for (size_t j = 2; j + 1 < last.operands.size(); j += 2) {
          successors[(uint32_t)bi].insert(last.operands[j + 1]);
        }
      }
    } else if (last.opcode == LLVMInstruction::Ret) {
      terminators[bi].kind = TerminatorInfo::Ret;
      terminators[bi].operands = last.operands;
    } else if (last.opcode == LLVMInstruction::Unreachable) {
      terminators[bi].kind = TerminatorInfo::Unreachable;
    }
  }

  // Build predecessor map
  std::map<uint32_t, std::set<uint32_t>> predecessors;
  for (auto &[from, succs] : successors) {
    for (uint32_t to : succs) {
      predecessors[to].insert(from);
    }
  }

  // Collect PHI info: block_idx -> list of (result_slot, type_id, [(value_id, pred_block_idx)])
  struct PhiIncoming {
    uint32_t value_id;
    uint32_t pred_block_idx;
  };
  struct PhiInfo {
    uint32_t result_slot;
    uint32_t type_id;
    std::vector<PhiIncoming> incoming;
  };
  std::map<uint32_t, std::vector<PhiInfo>> phi_info_per_block;

  // Pre-scan instructions to find PHI result_slot values
  // We need value_counter to match what emitInstruction uses
  // instruction_start_value already accounts for fn_constants + block labels
  {
    uint32_t vc = fn.instruction_start_value;
    for (size_t bi = 0; bi < fn.blocks.size(); bi++) {
      auto &block = fn.blocks[bi];
      for (auto &inst : block.instructions) {
        if (inst.opcode == LLVMInstruction::PHI) {
          PhiInfo pi;
          pi.result_slot = vc;
          pi.type_id = inst.type_id;
          for (size_t j = 0; j + 1 < inst.operands.size(); j += 2) {
            PhiIncoming inc;
            inc.value_id = inst.operands[j];
            uint32_t pred_block_val_id = inst.operands[j + 1];
            auto it = block_value_to_index.find(pred_block_val_id);
            if (it == block_value_to_index.end()) {
              DXTRACE("DXIL PHI: cannot resolve block value %u to index", pred_block_val_id);
              inc.pred_block_idx = UINT32_MAX;
            } else {
              inc.pred_block_idx = it->second;
            }
            pi.incoming.push_back(inc);
          }
          phi_info_per_block[(uint32_t)bi].push_back(pi);
        }
        // Advance value counter same as emitInstruction does
        switch (inst.opcode) {
        case LLVMInstruction::Ret:
        case LLVMInstruction::Br:
        case LLVMInstruction::Switch:
        case LLVMInstruction::Unreachable:
        case LLVMInstruction::Store:
          break;
        default:
          vc++;
          break;
        }
      }
    }
  }

  // Pre-declare PHI variables at function scope
  for (auto &[bi, phis] : phi_info_per_block) {
    for (auto &pi : phis) {
      std::string name = emitValue(pi.result_slot);
      std::string default_val = "0";
      if (pi.type_id > 0 && pi.type_id < module.types.size()) {
        auto &ty = module.types[pi.type_id];
        if (ty.kind == LLVMType::Float) default_val = "0.0f";
        else if (ty.kind == LLVMType::Double) default_val = "0.0";
        else if (ty.kind == LLVMType::Vector) {
          if (!ty.subtypes.empty() && ty.subtypes[0].kind == LLVMType::Integer)
            default_val = "uint4(0)";
          else
            default_val = "float4(0)";
        }
      }
      os << "  auto " << name << " = " << default_val << "; // phi pre-decl\n";
    }
  }

  uint32_t value_counter = fn.instruction_start_value;
  ctx.instruction_start_value = fn.instruction_start_value;

  auto resolveValue = [&](uint32_t idx) -> std::string {
    if (idx < ctx.value_table.size() && !ctx.value_table[idx].empty()) {
      const auto &v = ctx.value_table[idx];
      if (v.find('.') != std::string::npos && !startsWith(v, "dx."))
        return v;
      if (startsWith(v, "agg(")) {
        auto parts = parseAggregateLiteral(v);
        std::string args;
        for (size_t p = 0; p < parts.size(); p++) {
          if (p) args += ",";
          args += parts[p];
        }
        return "int" + std::to_string(parts.size()) + "(" + args + ")";
      }
      return v;
    }
    for (auto &c : ctx.mod.constants) {
      if (c.id == idx) {
        if (!c.constant_data.empty()) return c.constant_data;
        break;
      }
    }
    if (ctx.current_fn) {
      for (auto &c : ctx.current_fn->constants) {
        if (c.id == idx) {
          if (!c.constant_data.empty()) return c.constant_data;
          break;
        }
      }
    }
    return emitValue(idx);
  };

  // Emit blocks with switch-based state machine (Metal doesn't support goto)
  bool needs_dispatch = fn.blocks.size() > 1;
  if (needs_dispatch) {
    os << "  int _block_state = 0;\n";
    os << "  for (;;) {\n";
    os << "    switch (_block_state) {\n";
  }

  for (size_t bi = 0; bi < fn.blocks.size(); bi++) {
    auto &block = fn.blocks[bi];

    if (needs_dispatch) {
      os << "    case " << bi << ": {\n";
    }

    for (size_t ii = 0; ii < block.instructions.size(); ii++) {
      auto &inst = block.instructions[ii];
      bool is_terminator = (inst.opcode == LLVMInstruction::Br ||
                            inst.opcode == LLVMInstruction::Switch ||
                            inst.opcode == LLVMInstruction::Ret ||
                            inst.opcode == LLVMInstruction::Unreachable);

      if (inst.opcode == LLVMInstruction::PHI) {
        ctx.value_table.resize(std::max((size_t)value_counter + 1, ctx.value_table.size()));
        ctx.value_type_ids.resize(std::max((size_t)value_counter + 1, ctx.value_type_ids.size()), 0);
        ctx.value_table[value_counter] = emitValue(value_counter);
        ctx.value_type_ids[value_counter] = inst.type_id;
        value_counter++;
        continue;
      }

      if (!is_terminator) {
        emitInstruction(ctx, inst, value_counter);
        continue;
      }

      // Before terminator: emit PHI assignments for successor blocks
      auto succ_it = successors.find((uint32_t)bi);
      if (succ_it != successors.end()) {
        for (uint32_t succ : succ_it->second) {
          auto phi_it = phi_info_per_block.find(succ);
          if (phi_it != phi_info_per_block.end()) {
            for (auto &pi : phi_it->second) {
              for (auto &inc : pi.incoming) {
                if (inc.pred_block_idx == (uint32_t)bi) {
                  std::string phi_name = emitValue(pi.result_slot);
                  std::string val = resolveValue(inc.value_id);
                  os << "    " << phi_name << " = " << val << "; // phi from bb" << bi << "\n";
                }
              }
            }
          }
        }
      }

      // Emit terminator
      if (inst.opcode == LLVMInstruction::Ret) {
        if (ctx.shader.kind == DxilShaderKind::Vertex) {
          os << "    return out;\n";
        } else if (ctx.shader.kind == DxilShaderKind::Pixel) {
          if (!inst.operands.empty()) {
            std::string ret = resolveValue(inst.operands[0]);
            os << "    result.color0 = float4(" << ret << ");\n";
          }
          os << "    return result;\n";
        } else {
          if (!inst.operands.empty()) {
            std::string ret = resolveValue(inst.operands[0]);
            os << "    return " << ret << ";\n";
          } else {
            os << "    return;\n";
          }
        }
      } else if (inst.opcode == LLVMInstruction::Br) {
        if (inst.operands.size() == 1) {
          uint32_t target = inst.operands[0];
          if (needs_dispatch) {
            os << "    _block_state = " << target << "; continue;\n";
          }
        } else if (inst.operands.size() >= 3) {
          std::string cond = resolveValue(inst.operands[0]);
          uint32_t true_bb = inst.operands[1];
          uint32_t false_bb = inst.operands[2];
          if (needs_dispatch) {
            os << "    if (" << cond << ") { _block_state = " << true_bb << "; continue; } else { _block_state = " << false_bb << "; continue; }\n";
          }
        }
      } else if (inst.opcode == LLVMInstruction::Switch) {
        if (inst.operands.size() >= 2) {
          std::string cond = resolveValue(inst.operands[0]);
          uint32_t default_bb = inst.operands[1];
          if (inst.operands.size() > 4) {
            os << "    { int _sv = (int)(" << cond << ");\n";
            for (size_t j = 2; j + 1 < inst.operands.size(); j += 2) {
              os << "    if (_sv == " << inst.operands[j] << ") { _block_state = " << inst.operands[j + 1] << "; continue; }\n";
            }
            os << "    _block_state = " << default_bb << "; continue; }\n";
          } else {
            os << "    _block_state = " << default_bb << "; continue;\n";
          }
        }
      } else if (inst.opcode == LLVMInstruction::Unreachable) {
        os << "    // unreachable\n";
      }

      if (needs_dispatch) {
        os << "    }\n";
      }
    }
  }

  if (needs_dispatch) {
    os << "    }\n";
    os << "    break;\n";
    os << "  }\n";
  }

  os << "}\n";

  MSLShader result;
  result.source = os.str();
  result.entry_point = shader.entry_point;
  result.tg_size[0] = module.num_threads[0];
  result.tg_size[1] = module.num_threads[1];
  result.tg_size[2] = module.num_threads[2];
  result.unsupported_intrinsics = ctx.unsupported_intrinsics;
  result.unsupported_opcodes = ctx.unsupported_opcodes;
  result.diagnostics = ctx.diagnostics;

  DXTRACE("DXILToMSL: generated %zu bytes of MSL unsupported_intrinsics=%u unsupported_opcodes=%u",
          result.source.size(), ctx.unsupported_intrinsics, ctx.unsupported_opcodes);

  return result;
}

}
