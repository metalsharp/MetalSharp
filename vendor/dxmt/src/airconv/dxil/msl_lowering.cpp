#include "msl_lowering.hpp"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace dxmt::dxil {

enum DXIntrinsicOpcode {
  // DXIL opcodes 0..3 are valid operation identifiers, so keep their
  // internal lowering IDs distinct from the zero sentinel used for an
  // unknown callee name.
  DXOP_TempRegLoad = 1001,
  DXOP_TempRegStore = 1002,
  DXOP_MinPrecXRegLoad = 1003,
  DXOP_MinPrecXRegStore = 1004,
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
  DXOP_TextureGather = 73,
  DXOP_TextureSample = 60,
  DXOP_TextureSampleBias = 61,
  DXOP_TextureSampleLevel = 62,
  DXOP_TextureSampleGrad = 63,
  DXOP_TextureSampleCmp = 64,
  DXOP_TextureSampleCmpLevelZero = 65,
  DXOP_Barrier = 80,
  DXOP_Discard = 82,
  DXOP_Unary = 13,
  DXOP_Binary = 14,
  DXOP_Tertiary = 15,
  DXOP_Dot2 = 54,
  DXOP_Dot3 = 55,
  DXOP_Dot4 = 56,
  DXOP_Dot2AddHalf = 162,
  DXOP_Dot4AddI8Packed = 163,
  DXOP_Dot4AddU8Packed = 164,
  DXOP_RawBufferLoad = 139,
  DXOP_RawBufferStore = 140,
  DXOP_BufferUpdateCounter = 70,
  DXOP_CheckAccessFullyMapped = 71,
  DXOP_GetDimensions = 72,
  DXOP_AtomicBinOp = 78,
  DXOP_AtomicCompareExchange = 79,
  DXOP_DerivCoarseX = 83,
  DXOP_DerivCoarseY = 84,
  DXOP_DerivFineX = 85,
  DXOP_DerivFineY = 86,
  DXOP_CalcLOD = 81,
  DXOP_LegacyF32ToF16 = 130,
  DXOP_LegacyF16ToF32 = 131,
  DXOP_LegacyDoubleToFloat = 132,
  DXOP_LegacyDoubleToSInt32 = 133,
  DXOP_LegacyDoubleToUInt32 = 134,
  DXOP_MakeDouble = 101,
  DXOP_SplitDouble = 102,
  DXOP_BitcastI16ToF16 = 124,
  DXOP_BitcastF16ToI16 = 125,
  DXOP_BitcastI32ToF32 = 126,
  DXOP_BitcastF32ToI32 = 127,
  DXOP_BitcastI64ToF64 = 128,
  DXOP_BitcastF64ToI64 = 129,
  DXOP_WaveIsFirstLane = 110,
  DXOP_WaveGetLaneIndex = 111,
  DXOP_WaveGetLaneCount = 112,
  DXOP_WaveAnyTrue = 113,
  DXOP_WaveAllTrue = 114,
  DXOP_WaveActiveAllEqual = 115,
  DXOP_WaveActiveBallot = 116,
  DXOP_WaveReadLaneAt = 117,
  DXOP_WaveReadLaneFirst = 118,
  DXOP_WaveActiveOp = 119,
  DXOP_WaveActiveBit = 120,
  DXOP_WavePrefixOp = 121,
  DXOP_WaveAllBitCount = 135,
  DXOP_WavePrefixBitCount = 136,
  DXOP_WaveMatch = 165,
  DXOP_WaveMultiPrefixOp = 166,
  DXOP_WaveMultiPrefixBitCount = 167,
  DXOP_QuadReadLaneAt = 122,
  DXOP_QuadOp = 123,
  DXOP_QuadVote = 222,
  DXOP_IsHelperLane = 221,
  DXOP_TextureStoreSample = 225,
  DXOP_TextureSampleCmpLevel = 224,
  DXOP_TextureSampleCmpGrad = 254,
  DXOP_TextureSampleCmpBias = 255,
  DXOP_TextureGatherCmp = 74,
  DXOP_TextureGatherRaw = 223,
  DXOP_Unpack4x8 = 219,
  DXOP_Pack4x8 = 220,
  DXOP_VectorReduceAnd = 309,
  DXOP_VectorReduceOr = 310,
  DXOP_FDot = 311,
  DXOP_SampleIndex = 90,
  DXOP_Coverage = 91,
  DXOP_InnerCoverage = 92,
  DXOP_EvalSnapped = 87,
  DXOP_EvalSampleIndex = 88,
  DXOP_EvalCentroid = 89,
  DXOP_AttributeAtVertex = 137,
  DXOP_WriteSamplerFeedback = 174,
  DXOP_WriteSamplerFeedbackBias = 175,
  DXOP_WriteSamplerFeedbackLevel = 176,
  DXOP_WriteSamplerFeedbackGrad = 177,
  DXOP_AllocateRayQuery = 178,
  DXOP_RayQueryTraceRayInline = 179,
  DXOP_RayQueryProceed = 180,
  DXOP_RayQueryAbort = 181,
  DXOP_RayQueryCommitNonOpaqueTriangleHit = 182,
  DXOP_RayQueryCommitProceduralPrimitiveHit = 183,
  DXOP_RayQueryCommittedStatus = 184,
  DXOP_RayQueryCandidateType = 185,
  DXOP_AllocateRayQuery2 = 258,
  DXOP_StartVertexLocation = 256,
  DXOP_StartInstanceLocation = 257,
  DXOP_ViewID = 138,
  // dx.op.isSpecialFloat carries the concrete operation (8..11) in its
  // opcode argument rather than in the intrinsic name.
  DXOP_SpecialFloat = 1000,
};

enum DXILMathOpcode {
  DXILOP_FAbs = 6, DXILOP_Saturate = 7, DXILOP_IsNaN = 8, DXILOP_IsInf = 9,
  DXILOP_IsFinite = 10, DXILOP_IsNormal = 11, DXILOP_Cos = 12, DXILOP_Sin = 13, DXILOP_Tan = 14,
  DXILOP_Acos = 15, DXILOP_Asin = 16, DXILOP_Atan = 17,
  DXILOP_Hcos = 18, DXILOP_Hsin = 19, DXILOP_Htan = 20,
  DXILOP_Exp = 21, DXILOP_Frc = 22, DXILOP_Log = 23,
  DXILOP_Sqrt = 24, DXILOP_Rsqrt = 25,
  DXILOP_Round_ne = 26, DXILOP_Round_ni = 27, DXILOP_Round_pi = 28, DXILOP_Round_z = 29,
  DXILOP_Bfrev = 30, DXILOP_Countbits = 31,
  DXILOP_FirstbitLo = 32, DXILOP_FirstbitHi = 33, DXILOP_FirstbitSHi = 34,
  DXILOP_FMax = 35, DXILOP_FMin = 36, DXILOP_IMax = 37, DXILOP_IMin = 38,
  DXILOP_UMax = 39, DXILOP_UMin = 40,
  DXILOP_IMul = 41, DXILOP_UMul = 42, DXILOP_UDiv = 43,
  DXILOP_UAddc = 44, DXILOP_USubb = 45,
  DXILOP_FMad = 46, DXILOP_Fma = 47, DXILOP_IMad = 48, DXILOP_UMad = 49,
  DXILOP_Msad = 50,
  DXILOP_Ibfe = 51, DXILOP_Ubfe = 52, DXILOP_Bfi = 53,
};

static const char *kMetalHeader = R"(#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace metal::raytracing;

static inline uint m12_update_counter(device atomic_uint* counter, int delta) {
  uint original = atomic_fetch_add_explicit(counter, uint(delta),
                                             memory_order_relaxed);
  return delta < 0 ? original - 1u : original;
}

)";

static std::string emitValue(uint32_t idx) {
    if (idx == 0xFFFFFFFF) return "undef";
    return "v" + std::to_string(idx);
}

static bool startsWith(const std::string &text, const char *prefix) {
    return text.rfind(prefix, 0) == 0;
}

static bool parseUnsignedLiteral(const std::string &text, uint32_t &value) {
    if (text.empty()) return false;
    char *end = nullptr;
    unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    value = (uint32_t)parsed;
    return true;
}

static bool parseDoubleLiteralBits(const std::string &text, uint64_t &bits) {
    if (text.empty()) return false;
    char *end = nullptr;
    double value = std::strtod(text.c_str(), &end);
    if (!end || *end != '\0') return false;
    std::memcpy(&bits, &value, sizeof(bits));
    return true;
}

static std::string formatDoubleBits(uint64_t bits) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%016llxul",
                  static_cast<unsigned long long>(bits));
    return buffer;
}

static std::string doubleLiteralExpression(const std::string &text) {
    uint64_t bits = 0;
    return parseDoubleLiteralBits(text, bits) ? formatDoubleBits(bits) : text;
}

static bool parseEmittedValueName(const std::string &name, uint32_t &idx) {
    if (name.size() < 2 || name[0] != 'v') return false;
    return parseUnsignedLiteral(name.substr(1), idx);
}

static std::vector<std::string> parseAggregateLiteral(const std::string &text) {
    std::vector<std::string> values;
    bool is_agg = startsWith(text, "agg(") && text.size() >= 5 && text.back() == ')';
    bool is_brace = !text.empty() && text[0] == '{' && text.back() == '}';
    size_t start = is_agg ? 4 : 1;
    if (!is_agg && !is_brace) {
        static const char *ctors[] = {
            "float2(", "float3(", "float4(",
            "int2(", "int3(", "int4(",
            "uint2(", "uint3(", "uint4(",
            "half2(", "half3(", "half4("
        };
        bool is_ctor = false;
        for (auto *ctor : ctors) {
            if (startsWith(text, ctor) && text.back() == ')') {
                start = std::strlen(ctor);
                is_ctor = true;
                break;
            }
        }
        if (!is_ctor) return values;
    }
    int depth = 0;
    size_t arg_start = start;
    for (size_t i = start; i < text.size() - 1; i++) {
        char c = text[i];
        if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if (c == ')' || c == ']' || c == '}') {
            if (depth > 0)
                depth--;
        } else if (c == ',' && depth == 0) {
            std::string val = text.substr(arg_start, i - arg_start);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
                val.erase(val.begin());
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
                val.pop_back();
            if (!val.empty()) values.push_back(val);
            arg_start = i + 1;
        }
    }
    if (arg_start < text.size() - 1) {
        std::string val = text.substr(arg_start, text.size() - arg_start - 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
            val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
            val.pop_back();
        if (!val.empty()) values.push_back(val);
    }
    return values;
}

static std::string ensureScalarIndex(const std::string &val) {
    auto parts = parseAggregateLiteral(val);
    if (!parts.empty()) return parts[0];
    if (startsWith(val, "buf") || startsWith(val, "tex") || startsWith(val, "samp") ||
        val.find("char*") != std::string::npos || val.find("char *") != std::string::npos)
        return "0";
    return val;
}

static const char *componentSuffix(uint32_t component) {
    switch (component & 3) {
    case 0: return ".x"; case 1: return ".y"; case 2: return ".z"; default: return ".w";
    }
}

static const char *componentName(uint32_t component) {
    switch (component & 3) {
    case 0: return "x"; case 1: return "y"; case 2: return "z"; default: return "w";
    }
}

static std::string varyingField(const char *base, uint32_t sig_id) {
    if (sig_id == 0) return std::string(base) + ".position";
    if (sig_id <= 8) return std::string(base) + ".v" + std::to_string(sig_id - 1);
    return std::string(base) + ".v7";
}

static std::string escapeName(const std::string &s) {
    if (s.empty()) return "_";
    std::string r;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_')
            r += c;
        else r += '_';
    }
    if (!r.empty() && r[0] >= '0' && r[0] <= '9') r = "_" + r;
    return r;
}

static uint32_t intrinsicIdFromCalleeName(const std::string &name) {
    if (name.size() < 6 || name[0] != 'd' || name[1] != 'x' || name[2] != '.' || name[3] != 'o' || name[4] != 'p' || name[5] != '.')
        return 0;
    const char *s = name.c_str() + 6;
    if (strncmp(s, "tempRegLoad.", 12) == 0) return DXOP_TempRegLoad;
    if (strncmp(s, "tempRegStore.", 13) == 0) return DXOP_TempRegStore;
    if (strncmp(s, "minPrecXRegLoad.", 16) == 0) return DXOP_MinPrecXRegLoad;
    if (strncmp(s, "minPrecXRegStore.", 17) == 0) return DXOP_MinPrecXRegStore;
    if (strncmp(s, "loadInput.", 10) == 0) return 4;
    if (strncmp(s, "storeOutput.", 12) == 0) return 5;
    if (strncmp(s, "createHandleFromBinding", 23) == 0) return 217;
    if (strncmp(s, "createHandleFromHeap", 20) == 0) return 218;
    if (strncmp(s, "createHandleForLib", 18) == 0) return 160;
    if (strncmp(s, "createHandle", 12) == 0) return 57;
    if (strncmp(s, "annotateHandle", 14) == 0) return 216;
    if (strncmp(s, "cbufferLoadLegacy.", 18) == 0) return 59;
    if (strncmp(s, "cbufferLoad.", 12) == 0) return 58;
    if (strncmp(s, "threadIdInGroup", 15) == 0) return 95;
    if (strncmp(s, "flattenedThreadIdInGroup", 24) == 0) return 96;
    if (strncmp(s, "threadId", 8) == 0) return 93;
    if (strncmp(s, "groupId", 7) == 0) return 94;
    if (strncmp(s, "bufferLoad.", 11) == 0) return 68;
    if (strncmp(s, "bufferStore.", 12) == 0) return 69;
    if (strncmp(s, "bufferUpdateCounter", 19) == 0) return 70;
    if (strncmp(s, "textureStoreSample.", 19) == 0) return 225;
    if (strncmp(s, "textureStore.", 13) == 0) return 67;
    if (strncmp(s, "textureLoad.", 12) == 0) return 66;
    if (strncmp(s, "textureGatherCmp.", 17) == 0) return 74;
    if (strncmp(s, "textureGatherRaw.", 17) == 0) return 223;
    if (strncmp(s, "textureGather.", 14) == 0) return 73;
    if (strncmp(s, "sampleCmpLevelZero.", 19) == 0) return 65;
    if (strncmp(s, "sampleCmpLevel.", 15) == 0) return DXOP_TextureSampleCmpLevel;
    if (strncmp(s, "sampleCmpGrad.", 14) == 0) return DXOP_TextureSampleCmpGrad;
    if (strncmp(s, "sampleCmpBias.", 14) == 0) return DXOP_TextureSampleCmpBias;
    if (strncmp(s, "sampleCmp.", 10) == 0) return DXOP_TextureSampleCmp;
    if (strncmp(s, "sampleGrad.", 11) == 0) return 63;
    if (strncmp(s, "sampleLevel.", 12) == 0) return 62;
    if (strncmp(s, "sampleBias.", 10) == 0) return 61;
    if (strncmp(s, "sample.", 7) == 0) return 60;
    if (strncmp(s, "unaryBits.", 10) == 0) return 13;
    if (strncmp(s, "unary.", 6) == 0) return 13;
    if (strncmp(s, "binary.", 7) == 0) return 14;
    if (strncmp(s, "tertiary.", 9) == 0) return 15;
    if (strncmp(s, "dot2.", 5) == 0) return 54;
    if (strncmp(s, "dot3.", 5) == 0) return 55;
    if (strncmp(s, "dot4.", 5) == 0) return 56;
    if (strncmp(s, "dot2AddHalf.", 12) == 0) return DXOP_Dot2AddHalf;
    if (strncmp(s, "dot4AddI8Packed.", 16) == 0) return DXOP_Dot4AddI8Packed;
    if (strncmp(s, "dot4AddU8Packed.", 16) == 0) return DXOP_Dot4AddU8Packed;
    if (strncmp(s, "barrier", 7) == 0) return 80;
    if (strncmp(s, "discard", 7) == 0) return DXOP_Discard;
    if (strncmp(s, "checkAccessFullyMapped", 22) == 0) return 71;
    if (strncmp(s, "getDimensions", 13) == 0) return 72;
    if (strncmp(s, "rawBufferLoadLegacy", 19) == 0) return 1025;
    if (strncmp(s, "rawBufferStoreLegacy", 20) == 0) return 1026;
    if (strncmp(s, "rawBufferVectorLoad", 19) == 0) return 303;
    if (strncmp(s, "rawBufferVectorStore", 20) == 0) return 304;
    if (strncmp(s, "rawBufferLoad", 13) == 0) return 139;
    if (strncmp(s, "rawBufferStore", 14) == 0) return 140;
    if (strncmp(s, "atomicCompareExchange", 21) == 0) return 79;
    if (strncmp(s, "atomicBinOp", 11) == 0) return 78;
    if (strncmp(s, "derivCoarseX", 12) == 0) return 83;
    if (strncmp(s, "derivCoarseY", 12) == 0) return 84;
    if (strncmp(s, "derivFineX", 10) == 0) return 85;
    if (strncmp(s, "derivFineY", 10) == 0) return 86;
    if (strncmp(s, "calculateLOD", 12) == 0 || strncmp(s, "calcLOD", 7) == 0) return 81;
    if (strncmp(s, "makeDouble", 10) == 0) return DXOP_MakeDouble;
    if (strncmp(s, "splitDouble", 11) == 0) return DXOP_SplitDouble;
    if (strncmp(s, "bitcastI16toF16", 15) == 0) return DXOP_BitcastI16ToF16;
    if (strncmp(s, "bitcastF16toI16", 15) == 0) return DXOP_BitcastF16ToI16;
    if (strncmp(s, "bitcastI32toF32", 15) == 0) return DXOP_BitcastI32ToF32;
    if (strncmp(s, "bitcastF32toI32", 15) == 0) return DXOP_BitcastF32ToI32;
    if (strncmp(s, "bitcastI64toF64", 15) == 0) return DXOP_BitcastI64ToF64;
    if (strncmp(s, "bitcastF64toI64", 15) == 0) return DXOP_BitcastF64ToI64;
    if (strncmp(s, "legacyF32ToF16", 14) == 0) return DXOP_LegacyF32ToF16;
    if (strncmp(s, "legacyF16ToF32", 14) == 0) return DXOP_LegacyF16ToF32;
    if (strncmp(s, "legacyDoubleToFloat", 19) == 0) return DXOP_LegacyDoubleToFloat;
    if (strncmp(s, "legacyDoubleToSInt32", 20) == 0) return DXOP_LegacyDoubleToSInt32;
    if (strncmp(s, "legacyDoubleToUInt32", 20) == 0) return DXOP_LegacyDoubleToUInt32;
    if (strncmp(s, "waveReadLaneFirst", 17) == 0) return 118;
    if (strncmp(s, "waveReadLaneAt", 14) == 0) return 117;
    if (strncmp(s, "waveIsFirstLane", 15) == 0) return 110;
    if (strncmp(s, "waveGetLaneIndex", 16) == 0) return 111;
    if (strncmp(s, "waveGetLaneCount", 16) == 0) return 112;
    if (strncmp(s, "waveAnyTrue", 11) == 0) return 113;
    if (strncmp(s, "waveAllTrue", 11) == 0) return 114;
    if (strncmp(s, "waveActiveAllEqual", 18) == 0) return 115;
    if (strncmp(s, "waveActiveBallot", 16) == 0) return 116;
    if (strncmp(s, "waveActiveOp", 12) == 0) return 119;
    if (strncmp(s, "waveActiveBit", 13) == 0) return 120;
    if (strncmp(s, "waveAllBitCount", 15) == 0) return 135;
    if (strncmp(s, "wavePrefixBitCount", 18) == 0) return 136;
    if (strncmp(s, "waveMatch", 9) == 0) return DXOP_WaveMatch;
    if (strncmp(s, "waveMultiPrefixOp", 17) == 0) return DXOP_WaveMultiPrefixOp;
    if (strncmp(s, "waveMultiPrefixBitCount", 23) == 0) return DXOP_WaveMultiPrefixBitCount;
    if (strncmp(s, "wavePrefixOp", 12) == 0) return 121;
    if (strncmp(s, "quadReadLaneAt", 14) == 0) return 122;
    if (strncmp(s, "quadOp", 6) == 0) return 123;
    if (strncmp(s, "quadVote", 8) == 0) return 222;
    if (strncmp(s, "isHelperLane", 12) == 0) return DXOP_IsHelperLane;
    if (strncmp(s, "writeSamplerFeedbackLevel", 25) == 0) return 176;
    if (strncmp(s, "writeSamplerFeedbackGrad", 24) == 0) return 177;
    if (strncmp(s, "writeSamplerFeedbackBias", 24) == 0) return 175;
    if (strncmp(s, "writeSamplerFeedback", 20) == 0) return 174;
    if (strncmp(s, "allocateRayQuery", 16) == 0) return DXOP_AllocateRayQuery;
    if (strncmp(s, "rayQuery_TraceRayInline", 23) == 0) return DXOP_RayQueryTraceRayInline;
    if (strncmp(s, "rayQuery_Proceed", 16) == 0) return DXOP_RayQueryProceed;
    if (strncmp(s, "rayQuery_Abort", 14) == 0) return DXOP_RayQueryAbort;
    if (strncmp(s, "rayQuery_CommitNonOpaqueTriangleHit", 35) == 0)
        return DXOP_RayQueryCommitNonOpaqueTriangleHit;
    if (strncmp(s, "rayQuery_CommitProceduralPrimitiveHit", 37) == 0)
        return DXOP_RayQueryCommitProceduralPrimitiveHit;
    if (strncmp(s, "rayQuery_StateMatrix", 20) == 0)
        return 186;
    if (strncmp(s, "rayQuery_StateVector", 20) == 0)
        return 193;
    if (strncmp(s, "rayQuery_StateScalar", 20) == 0)
        return DXOP_RayQueryCandidateType;
    if (strncmp(s, "startVertexLocation", 19) == 0)
        return DXOP_StartVertexLocation;
    if (strncmp(s, "startInstanceLocation", 21) == 0)
        return DXOP_StartInstanceLocation;
    if (strncmp(s, "viewID", 6) == 0)
        return DXOP_ViewID;
    if (strncmp(s, "isSpecialFloat.", 14) == 0) return DXOP_SpecialFloat;
    if (strncmp(s, "cycleCounterLegacy", 18) == 0) return 109;
    if (strncmp(s, "texture2DMSGetSamplePosition", 27) == 0) return 75;
    if (strncmp(s, "renderTargetGetSamplePosition", 29) == 0) return 76;
    if (strncmp(s, "renderTargetGetSampleCount", 26) == 0) return 77;
    if (strncmp(s, "unpack4x8.", 10) == 0) return DXOP_Unpack4x8;
    if (strncmp(s, "pack4x8.", 8) == 0) return DXOP_Pack4x8;
    if (strncmp(s, "vectorReduceAnd.", 17) == 0) return DXOP_VectorReduceAnd;
    if (strncmp(s, "vectorReduceOr.", 16) == 0) return DXOP_VectorReduceOr;
    if (strncmp(s, "fDot.", 5) == 0) return DXOP_FDot;
    if (strncmp(s, "sampleIndex", 11) == 0) return DXOP_SampleIndex;
    if (strncmp(s, "coverage", 8) == 0) return DXOP_Coverage;
    if (strncmp(s, "innerCoverage", 13) == 0) return DXOP_InnerCoverage;
    if (strncmp(s, "evalSnapped", 11) == 0) return DXOP_EvalSnapped;
    if (strncmp(s, "evalSampleIndex", 15) == 0) return DXOP_EvalSampleIndex;
    if (strncmp(s, "evalCentroid", 12) == 0) return DXOP_EvalCentroid;
    if (strncmp(s, "attributeAtVertex", 17) == 0) return DXOP_AttributeAtVertex;
    return 0;
}

static uint32_t canonicalDXIntrinsicId(uint32_t opcode) {
    switch (opcode) {
    case 0: return DXOP_TempRegLoad;
    case 1: return DXOP_TempRegStore;
    case 2: return DXOP_MinPrecXRegLoad;
    case 3: return DXOP_MinPrecXRegStore;
    default: return opcode;
    }
}

static bool isOpcodePrefixedDXIntrinsic(uint32_t opcode) {
    switch (opcode) {
    case 0:
    case 1:
    case 2:
    case 3:
    case DXOP_LoadInput:
    case DXOP_StoreOutput:
    case DXOP_CreateHandle:
    case DXOP_CreateHandleFromBinding:
    case DXOP_CreateHandleFromHeap:
    case DXOP_CreateHandleForLib:
    case DXOP_AnnotateHandle:
    case DXOP_CBufferLoad:
    case DXOP_CBufferLoadLegacy:
    case DXOP_ThreadId:
    case DXOP_GroupId:
    case DXOP_ThreadIDInGroup:
    case DXOP_FlattenedThreadIDInGroup:
    case DXOP_TextureSample:
    case DXOP_TextureSampleBias:
    case DXOP_TextureSampleLevel:
    case DXOP_TextureSampleGrad:
    case DXOP_TextureSampleCmpGrad:
    case DXOP_TextureSampleCmpBias:
    case DXOP_TextureLoad:
    case DXOP_TextureStore:
    case DXOP_TextureGather:
    case DXOP_TextureStoreSample:
    case 75:
    case 76:
    case 77:
    case DXOP_Unpack4x8:
    case DXOP_Pack4x8:
    case DXOP_VectorReduceAnd:
    case DXOP_VectorReduceOr:
    case DXOP_FDot:
    case DXOP_SampleIndex:
    case DXOP_Coverage:
    case DXOP_InnerCoverage:
    case DXOP_EvalSnapped:
    case DXOP_EvalSampleIndex:
    case DXOP_EvalCentroid:
    case DXOP_AttributeAtVertex:
    case DXOP_BufferLoad:
    case DXOP_BufferStore:
    case DXOP_RawBufferLoad:
    case DXOP_RawBufferStore:
    case DXOP_MakeDouble:
    case DXOP_SplitDouble:
    case DXOP_BitcastI16ToF16:
    case DXOP_BitcastF16ToI16:
    case DXOP_BitcastI32ToF32:
    case DXOP_BitcastF32ToI32:
    case DXOP_BitcastI64ToF64:
    case DXOP_BitcastF64ToI64:
    case DXOP_Dot2AddHalf:
    case DXOP_Dot4AddI8Packed:
    case DXOP_Dot4AddU8Packed:
    case DXOP_WaveIsFirstLane:
    case DXOP_WaveGetLaneIndex:
    case DXOP_WaveGetLaneCount:
    case DXOP_WaveAnyTrue:
    case DXOP_WaveAllTrue:
    case DXOP_WaveActiveAllEqual:
    case DXOP_WaveActiveBallot:
    case DXOP_WaveReadLaneAt:
    case DXOP_WaveReadLaneFirst:
    case DXOP_WaveActiveOp:
    case DXOP_WaveActiveBit:
    case DXOP_WaveAllBitCount:
    case DXOP_WavePrefixBitCount:
    case DXOP_WaveMatch:
    case DXOP_WaveMultiPrefixOp:
    case DXOP_WaveMultiPrefixBitCount:
    case DXOP_WavePrefixOp:
    case DXOP_LegacyF32ToF16:
    case DXOP_LegacyF16ToF32:
    case DXOP_LegacyDoubleToFloat:
    case DXOP_LegacyDoubleToSInt32:
    case DXOP_LegacyDoubleToUInt32:
    case DXOP_QuadReadLaneAt:
    case DXOP_QuadOp:
    case DXOP_QuadVote:
    case DXOP_IsHelperLane:
    case DXOP_WriteSamplerFeedback:
    case DXOP_WriteSamplerFeedbackBias:
    case DXOP_WriteSamplerFeedbackLevel:
    case DXOP_WriteSamplerFeedbackGrad:
    case DXOP_AllocateRayQuery:
    case DXOP_RayQueryTraceRayInline:
    case DXOP_RayQueryProceed:
    case DXOP_RayQueryAbort:
    case DXOP_RayQueryCommitNonOpaqueTriangleHit:
    case DXOP_RayQueryCommitProceduralPrimitiveHit:
    case DXOP_RayQueryCommittedStatus:
    case DXOP_RayQueryCandidateType:
    case DXOP_AllocateRayQuery2:
    case DXOP_StartVertexLocation:
    case DXOP_StartInstanceLocation:
    case DXOP_ViewID:
    case 186:
    case 187:
    case 188:
    case 189:
    case 190:
    case 191:
    case 192:
    case 193:
    case 194:
    case 195:
    case 196:
    case 197:
    case 198:
    case 199:
    case 200:
    case 201:
    case 202:
    case 203:
    case 204:
    case 205:
    case 206:
    case 207:
    case 208:
    case 209:
    case 210:
    case 211:
    case 212:
    case 214:
    case 215:
        return true;
    default:
        return false;
    }
}

static std::string emitTypeName(const MSLType &t) {
    if (t.kind == MSLTypeKind::Struct || t.kind == MSLTypeKind::Unknown)
        return "auto";
    return DXILIRBuilder::mslTypeName(t);
}

static std::string defaultForType(const MSLType &t) {
    switch (t.kind) {
    case MSLTypeKind::Bool: return "false";
    case MSLTypeKind::Float: return "0.0f";
    case MSLTypeKind::Float2: return "float2(0.0f)";
    case MSLTypeKind::Float3: return "float3(0.0f)";
    case MSLTypeKind::Float4: return "float4(0.0f)";
    case MSLTypeKind::Int: return "0";
    case MSLTypeKind::Int2: return "int2(0)";
    case MSLTypeKind::Int3: return "int3(0)";
    case MSLTypeKind::Int4: return "int4(0)";
    case MSLTypeKind::UInt: return "0u";
    case MSLTypeKind::UInt2: return "uint2(0)";
    case MSLTypeKind::UInt3: return "uint3(0)";
    case MSLTypeKind::UInt4: return "uint4(0)";
    case MSLTypeKind::Texture2D:
    case MSLTypeKind::Texture2DArray:
    case MSLTypeKind::Texture3D:
    case MSLTypeKind::TextureCube:
    case MSLTypeKind::Texture2DMS:
    case MSLTypeKind::RWTexture2D:
    case MSLTypeKind::RWTexture2DArray:
    case MSLTypeKind::RWTexture3D:
        return "tex0";
    case MSLTypeKind::Sampler:
        return "samp0";
    case MSLTypeKind::InstanceAccelerationStructure:
        return "as16";
    case MSLTypeKind::RayQuery:
        return "{}";
    case MSLTypeKind::LongVector:
        return "{}";
    default: return "0";
    }
}

static MSLType aggregateFallbackType(const std::vector<std::string> &parts) {
    bool has_float = false;
    bool has_unsigned = false;
    for (const auto &part : parts) {
        if (part.find('.') != std::string::npos || part.find('f') != std::string::npos ||
            part.find("inf") != std::string::npos || part.find("nan") != std::string::npos) {
            has_float = true;
            break;
        }
        if (!part.empty() && part.back() == 'u')
            has_unsigned = true;
    }

    if (parts.size() > 4) {
        MSLType result;
        result.kind = MSLTypeKind::LongVector;
        result.vector_width = static_cast<uint32_t>(parts.size());
        result.vector_element_kind = has_float
                                          ? MSLTypeKind::Float
                                          : has_unsigned
                                                ? MSLTypeKind::UInt
                                                : MSLTypeKind::Int;
        return result;
    }

    size_t count = std::min<size_t>(std::max<size_t>(parts.size(), 1), 4);
    if (has_float) {
        switch (count) {
        case 2: return {MSLTypeKind::Float2, 0, {}};
        case 3: return {MSLTypeKind::Float3, 0, {}};
        case 4: return {MSLTypeKind::Float4, 0, {}};
        default: return {MSLTypeKind::Float, 0, {}};
        }
    }
    if (has_unsigned) {
        switch (count) {
        case 2: return {MSLTypeKind::UInt2, 0, {}};
        case 3: return {MSLTypeKind::UInt3, 0, {}};
        case 4: return {MSLTypeKind::UInt4, 0, {}};
        default: return {MSLTypeKind::UInt, 0, {}};
        }
    }
    switch (count) {
    case 2: return {MSLTypeKind::Int2, 0, {}};
    case 3: return {MSLTypeKind::Int3, 0, {}};
    case 4: return {MSLTypeKind::Int4, 0, {}};
    default: return {MSLTypeKind::Int, 0, {}};
    }
}

static bool isAggregateLiteralText(const std::string &text) {
    return startsWith(text, "agg(") && text.size() >= 5 && text.back() == ')';
}

static std::string aggregateConstructor(const std::string &literal, MSLType type = {}) {
    auto parts = parseAggregateLiteral(literal);
    if (parts.empty()) return literal;
    if (!DXILIRBuilder::isVectorType(type) &&
        !DXILIRBuilder::isLongVectorType(type))
        type = aggregateFallbackType(parts);

    std::string type_name = emitTypeName(type);
    if (type_name.empty() || type_name == "auto")
        type_name = emitTypeName(aggregateFallbackType(parts));

    std::string args;
    auto scalarize_vector_part = [](const std::string &part) {
        std::string trimmed = part;
        while (!trimmed.empty() && std::isspace((unsigned char)trimmed.front()))
            trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back()))
            trimmed.pop_back();

        static const char *constructors[] = {
            "float2(", "float3(", "float4(",
            "int2(", "int3(", "int4(",
            "uint2(", "uint3(", "uint4("
        };
        for (const char *ctor : constructors) {
            if (startsWith(trimmed, ctor))
                return "(" + trimmed + ").x";
        }
        return part;
    };

    const bool signed_32 =
        type.kind == MSLTypeKind::Int || type.kind == MSLTypeKind::Int2 ||
        type.kind == MSLTypeKind::Int3 || type.kind == MSLTypeKind::Int4 ||
        (type.kind == MSLTypeKind::LongVector &&
         type.vector_element_kind == MSLTypeKind::Int);
    auto normalize_signed_literal = [&](const std::string &part) {
        if (!signed_32)
            return part;
        char *end = nullptr;
        const unsigned long long raw = std::strtoull(part.c_str(), &end, 10);
        if (!end || *end != '\0' || raw > 0xffffffffull)
            return part;
        return std::to_string(static_cast<int32_t>(static_cast<uint32_t>(raw)));
    };
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) args += ", ";
        const std::string part = DXILIRBuilder::isVectorType(type)
                                      ? scalarize_vector_part(parts[i])
                                      : parts[i];
        args += normalize_signed_literal(part);
    }
    if (DXILIRBuilder::isLongVectorType(type))
        return type_name + "{{" + args + "}}";
    return type_name + "(" + args + ")";
}

static std::string normalizeAggregateExpressions(const std::string &expr, MSLType preferred_type = {}) {
    if (isAggregateLiteralText(expr))
        return aggregateConstructor(expr, preferred_type);

    std::string out;
    size_t pos = 0;
    while (pos < expr.size()) {
        size_t start = expr.find("agg(", pos);
        if (start == std::string::npos) {
            out += expr.substr(pos);
            break;
        }
        out += expr.substr(pos, start - pos);
        int depth = 0;
        size_t end = start;
        for (; end < expr.size(); end++) {
            if (expr[end] == '(') depth++;
            else if (expr[end] == ')') {
                depth--;
                if (depth == 0) {
                    end++;
                    break;
                }
            }
        }
        if (depth != 0) {
            out += expr.substr(start);
            break;
        }
        out += aggregateConstructor(expr.substr(start, end - start));
        pos = end;
    }
    return out;
}

static std::string typedDecl(const std::string &name, const MSLType &t) {
    return emitTypeName(t) + " " + name;
}

struct DescriptorRangePlan {
    enum class Kind { SRV, UAV, CBV, Sampler } kind = Kind::SRV;
    uint32_t register_space = 0;
    uint32_t lower_bound = 0;
    uint32_t count = 1;
    uint32_t resource_kind = 0;
    uint32_t element_type = 0;
    uint32_t element_stride = 0;
    uint32_t sample_count = 1;
};

struct ResourceHandleRecord {
    DescriptorRangePlan::Kind kind = DescriptorRangePlan::Kind::SRV;
    uint32_t resource_class = 0;
    uint32_t register_space = 0;
    uint32_t lower_bound = 0;
    uint32_t binding_index = 0;
    uint32_t binding_count = 1;
    uint32_t resource_kind = 0;
    uint32_t element_type = 0;
    uint32_t element_stride = 0;
    uint32_t sample_count = 1;
    bool non_uniform = false;
    bool direct_heap = false;
    std::string dynamic_index;
};

struct BindingPlan {
    std::vector<DescriptorRangePlan> ranges;
    uint32_t direct_buffer_count = 31;
    uint32_t direct_texture_count = 8;
    uint32_t direct_sampler_count = 4;
};

struct LowerContext {
    std::ostringstream &os;
    const LLVMModule &mod;
    const DxilParsedShader &shader;
    const MSLLoweringOptions &options;
    std::vector<std::string> value_table;
    std::vector<MSLType> value_types;
    std::vector<ValueRole> value_roles;
    std::unordered_map<uint32_t, std::string> buffer_origin;
    // Retain private alloca/GEP pointee types so signless LLVM i32 loads
    // preserve the signed long-vector lane kind for later conversions.
    std::unordered_map<uint32_t, MSLType> pointer_pointee_types;
    std::unordered_map<uint32_t, uint32_t> vector_extract_origin;
    std::unordered_map<uint32_t, ResourceHandleRecord> resource_handles;
    std::optional<ResourceHandleRecord> pending_handle;
    std::string last_buffer_handle;
    std::unordered_map<std::string, std::string> local_values;
    std::vector<std::string> diagnostics;
    std::unordered_map<uint32_t, std::string> function_decls;
    std::set<std::string> predeclared_names;
    std::set<uint32_t> predeclared_allocas;
    std::unordered_map<std::string, MSLType> predeclared_types;
    BindingPlan binding_plan;
    uint32_t next_binding = 0;
    uint32_t unsupported_intrinsics = 0;
    uint32_t unsupported_opcodes = 0;
    uint32_t current_result_id = UINT32_MAX;
    bool uses_atomic32_emulation = false;
    uint32_t instruction_start_value = 0;
    const LLVMFunction *current_fn = nullptr;
    bool uses_thread_id = false;
    bool uses_group_id = false;
    bool uses_group_thread_id = false;
    bool uses_group_size = false;
    std::set<uint32_t> vertex_input_ids;
    std::set<uint32_t> group_i64_globals;
    bool vertex_has_float_load_input = false;
    bool vertex_procedural_fullscreen_fallback = false;
    bool compute_wave_shader = false;
    bool compute_raw_gather_shader = false;
    bool compute_texture_store_shader = false;
    bool texture_store_sample_shader = false;
    std::set<uint32_t> writable_msaa_texture_slots;
    bool sample_cmp_shader = false;
    bool compute_sample_cmp_shader = false;
    std::set<uint32_t> comparison_texture_slots;
    bool compute_texture_sample_shader = false;
    bool uses_atomic64_emulation = false;
    bool uses_group_atomic64_emulation = false;
    bool uses_double_emulation = false;
    bool uses_sample_index = false;
    bool uses_coverage = false;
    bool uses_interpolation = false;
    bool uses_sampler_feedback = false;
    bool uses_temp_registers = false;
};

// DXIL ResourceKind values are part of the public DXIL ABI.  Keep the
// dimension test in one place so descriptor declarations and intrinsic
// lowering cannot silently fall back to a 2D texture when resource metadata is
// available.
static bool isTextureResourceKind(uint32_t resource_kind) {
    return resource_kind >= 1u && resource_kind <= 9u;
}

static bool isAccelerationStructureResourceKind(uint32_t resource_kind) {
    return resource_kind == 16u; // DXIL ResourceKind::RTAccelerationStructure
}

static bool isTextureArrayResourceKind(uint32_t resource_kind) {
    return resource_kind == 6u || resource_kind == 7u ||
           resource_kind == 8u || resource_kind == 9u;
}

static bool isTextureMSAAResourceKind(uint32_t resource_kind) {
    return resource_kind == 3u || resource_kind == 8u;
}

static bool isTexture3DResourceKind(uint32_t resource_kind) {
    return resource_kind == 4u;
}

static bool isTextureCubeResourceKind(uint32_t resource_kind) {
    return resource_kind == 5u || resource_kind == 9u;
}

// DXIL ElementType values are defined by the resource metadata ABI.  The
// integer element types are 1..7 (I1/I16/U16/I32/U32/I64/U64); normalized and
// floating types are represented as float texture components in Metal.
static bool isIntegerResourceElementType(uint32_t element_type) {
    return element_type >= 1u && element_type <= 7u;
}

static bool isSignedResourceElementType(uint32_t element_type) {
    return element_type == 1u || element_type == 2u ||
           element_type == 4u || element_type == 6u;
}

static uint32_t resourceElementTypeForTextureSlot(const LowerContext &ctx,
                                                  uint32_t slot) {
    uint32_t srv_type = 0;
    uint32_t uav_type = 0;
    for (const auto &range : ctx.binding_plan.ranges) {
        if ((range.kind != DescriptorRangePlan::Kind::SRV &&
             range.kind != DescriptorRangePlan::Kind::UAV) ||
            slot < range.lower_bound ||
            slot - range.lower_bound >= range.count ||
            !isTextureResourceKind(range.resource_kind))
            continue;
        if (range.kind == DescriptorRangePlan::Kind::UAV)
            uav_type = range.element_type;
        else
            srv_type = range.element_type;
    }
    return uav_type ? uav_type : srv_type;
}

static uint32_t resourceElementTypeForHandle(const LowerContext &ctx,
                                              uint32_t handle_id) {
    auto it = ctx.resource_handles.find(handle_id);
    return it == ctx.resource_handles.end() ? 0u : it->second.element_type;
}

static uint32_t resourceKindForTextureSlot(const LowerContext &ctx,
                                           uint32_t slot) {
    uint32_t srv_kind = 0;
    uint32_t uav_kind = 0;
    for (const auto &range : ctx.binding_plan.ranges) {
        if ((range.kind != DescriptorRangePlan::Kind::SRV &&
             range.kind != DescriptorRangePlan::Kind::UAV) ||
            slot < range.lower_bound ||
            slot - range.lower_bound >= range.count ||
            !isTextureResourceKind(range.resource_kind))
            continue;
        if (range.kind == DescriptorRangePlan::Kind::UAV)
            uav_kind = range.resource_kind;
        else
            srv_kind = range.resource_kind;
    }
    // The direct ABI has one texture slot namespace even though D3D12 keeps
    // SRV and UAV register namespaces independent.  Match the existing
    // preference for the UAV declaration when both namespaces use a slot.
    return uav_kind ? uav_kind : srv_kind;
}

static bool accelerationStructureAtBufferSlot(const LowerContext &ctx,
                                               uint32_t slot) {
    for (const auto &range : ctx.binding_plan.ranges) {
        if (range.kind != DescriptorRangePlan::Kind::SRV ||
            !isAccelerationStructureResourceKind(range.resource_kind))
            continue;
        for (uint32_t i = 0; i < range.count; ++i) {
            if (range.lower_bound + i + 16u == slot)
                return true;
        }
    }
    return false;
}

static bool textureSlotHasRangeKind(const LowerContext &ctx, uint32_t slot,
                                    DescriptorRangePlan::Kind kind) {
    for (const auto &range : ctx.binding_plan.ranges) {
        if (range.kind == kind && isTextureResourceKind(range.resource_kind) &&
            slot >= range.lower_bound &&
            slot - range.lower_bound < range.count)
            return true;
    }
    return false;
}

static std::string textureBindingType(uint32_t resource_kind, bool writable,
                                      bool integer, bool sampled,
                                      bool writable_msaa_compat = false,
                                      bool signed_integer = false,
                                      bool allow_integer_sampling = false) {
    const char *element = integer ? (signed_integer ? "int" : "uint") : "float";
    // Metal cannot filter integer textures.  A typed integer SRV therefore
    // remains a read-only texture even when the shader's operation family is
    // otherwise a sampled-texture family.
    const char *access = writable ? "read_write"
                                  : (sampled && (!integer || allow_integer_sampling)
                                         ? "sample"
                                         : "read");
    if (writable_msaa_compat && isTextureMSAAResourceKind(resource_kind))
        return "texture2d_array<" + std::string(element) +
               ", access::read_write>";

    switch (resource_kind) {
    case 1u: return "texture2d<" + std::string(element) + ", access::" + access + ">";
    case 2u: return "texture2d<" + std::string(element) + ", access::" + access + ">";
    case 3u: return "texture2d_ms<" + std::string(element) + ", access::read>";
    case 4u: return "texture3d<" + std::string(element) + ", access::" + access + ">";
    case 5u: return "texturecube<" + std::string(element) + ", access::" + access + ">";
    case 6u: return "texture2d_array<" + std::string(element) + ", access::" + access + ">";
    case 7u: return "texture2d_array<" + std::string(element) + ", access::" + access + ">";
    case 8u: return "texture2d_ms_array<" + std::string(element) + ", access::read>";
    case 9u: return "texturecube_array<" + std::string(element) + ", access::" + access + ">";
    default: return "texture2d<" + std::string(element) + ", access::" + access + ">";
    }
}

static std::string depthTextureBindingType(uint32_t resource_kind) {
    if (resource_kind == 7u || resource_kind == 6u)
        return "depth2d_array<float, access::sample>";
    if (resource_kind == 5u)
        return "depthcube<float, access::sample>";
    if (resource_kind == 9u)
        return "depthcube_array<float, access::sample>";
    return "depth2d<float, access::sample>";
}

static std::string vertexPullField(LowerContext &ctx, uint32_t sig_id) {
    return MSLVertexPullExpression(sig_id, ctx.options);
}

static void recordDiagnostic(LowerContext &ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ctx.diagnostics.push_back(buf);
}

static void emitBindings(LowerContext &ctx) {
    auto &os = ctx.os;
    if (ctx.shader.kind == DxilShaderKind::Compute) {
        ctx.uses_thread_id = true;
        ctx.uses_group_id = true;
        ctx.uses_group_thread_id = true;
        ctx.uses_group_size = true;
    }
    os << "\n";
}

static const char *descriptorRangeKindName(DescriptorRangePlan::Kind kind) {
    switch (kind) {
    case DescriptorRangePlan::Kind::SRV: return "srv";
    case DescriptorRangePlan::Kind::UAV: return "uav";
    case DescriptorRangePlan::Kind::CBV: return "cbv";
    case DescriptorRangePlan::Kind::Sampler: return "sampler";
    }
    return "unknown";
}

static void emitBindingManifest(LowerContext &ctx) {
    auto &os = ctx.os;
    os << "// metalsharp.binding_manifest.v1\n";
    os << "// direct_buffers=" << ctx.binding_plan.direct_buffer_count
       << " direct_textures=" << ctx.binding_plan.direct_texture_count
       << " direct_samplers=" << ctx.binding_plan.direct_sampler_count << "\n";
    if (ctx.binding_plan.ranges.empty()) {
        os << "// range none\n\n";
        return;
    }
    for (const auto &range : ctx.binding_plan.ranges) {
        os << "// range kind=" << descriptorRangeKindName(range.kind)
           << " space=" << range.register_space
           << " lower=" << range.lower_bound
           << " count=" << range.count
           << " resource_kind=" << range.resource_kind
           << " element_type=" << range.element_type
           << " sample_count=" << range.sample_count
           << " stride=" << range.element_stride << "\n";
    }
    os << "\n";
}

static uint32_t directBufferBindingIndex(const LowerContext &ctx,
                                         const ResourceHandleRecord &handle,
                                         const char *target_prefix);

static void emitDefaultVertexVaryingWrites(std::ostream &os,
                                           bool procedural_fullscreen,
                                           bool has_draw_args,
                                           bool has_shading_rate_output,
                                           bool has_viewport_index_output,
                                           bool has_render_target_array_index_output) {
    os << "  out.position = float4(0.0, 0.0, 0.0, 1.0);\n";
    for (uint32_t i = 0; i < 8; i++)
        os << "  out.v" << i << " = float4(0.0);\n";
    for (uint32_t i = 0; i < 4; i++)
        os << "  out.uv" << i << " = float2(0.0);\n";
    for (uint32_t i = 0; i < 4; i++)
        os << "  out.color" << i << " = float4(0.0);\n";
    if (has_shading_rate_output)
        os << "  out.shading_rate = 0u;\n";
    if (has_viewport_index_output)
        os << "  out.viewport_array_index = 0u;\n";
    if (has_render_target_array_index_output)
        os << "  out.render_target_array_index = 0u;\n";
    if (procedural_fullscreen) {
        if (has_draw_args)
            os << "  uint m12_draw_vcount = m12_draw_vertex_count(buf29, buf30);\n";
        else
            os << "  uint m12_draw_vcount = 0u;\n";
        os << "  bool m12_use_strip_quad = m12_draw_vcount == 4u;\n";
        os << "  uint m12_strip_vid = vid & 3u;\n";
        os << "  uint m12_tri_vid = min(vid, 2u);\n";
        os << "  float2 m12_strip_uv = m12_strip_vid == 0u ? float2(0.0, 0.0) : (m12_strip_vid == 1u ? float2(1.0, 0.0) : (m12_strip_vid == 2u ? float2(0.0, 1.0) : float2(1.0, 1.0)));\n";
        os << "  float2 m12_strip_pos = m12_strip_vid == 0u ? float2(-1.0, 1.0) : (m12_strip_vid == 1u ? float2(1.0, 1.0) : (m12_strip_vid == 2u ? float2(-1.0, -1.0) : float2(1.0, -1.0)));\n";
        os << "  float2 m12_tri_uv = m12_tri_vid == 0u ? float2(0.0, 0.0) : (m12_tri_vid == 1u ? float2(0.0, 2.0) : float2(2.0, 0.0));\n";
        os << "  float2 m12_tri_pos = m12_tri_vid == 0u ? float2(-1.0, 1.0) : (m12_tri_vid == 1u ? float2(-1.0, -3.0) : float2(3.0, 1.0));\n";
        os << "  float2 m12_fullscreen_uv = m12_use_strip_quad ? m12_strip_uv : m12_tri_uv;\n";
        os << "  float2 m12_fullscreen_pos = m12_use_strip_quad ? m12_strip_pos : m12_tri_pos;\n";
        os << "  out.position = float4(m12_fullscreen_pos, 0.0, 1.0);\n";
        os << "  out.v1 = float4(m12_fullscreen_uv, 0.0, 0.0);\n";
    }
}

static const char *tempRegisterStorage(const MSLType &type) {
    switch (type.kind) {
    case MSLTypeKind::Bool:
        return "m12_temp_bool";
    case MSLTypeKind::Int:
    case MSLTypeKind::Int2:
    case MSLTypeKind::Int3:
    case MSLTypeKind::Int4:
    case MSLTypeKind::Short:
        return "m12_temp_int";
    case MSLTypeKind::UInt:
    case MSLTypeKind::UInt2:
    case MSLTypeKind::UInt3:
    case MSLTypeKind::UInt4:
    case MSLTypeKind::UShort:
        return "m12_temp_uint";
    case MSLTypeKind::Double:
        return "m12_temp_ulong";
    case MSLTypeKind::Long:
        return "m12_temp_long";
    case MSLTypeKind::Half:
    case MSLTypeKind::Float:
    case MSLTypeKind::Float2:
    case MSLTypeKind::Float3:
    case MSLTypeKind::Float4:
    default:
        return "m12_temp_float";
    }
}

static void emitTempRegisterDeclarations(LowerContext &ctx) {
    if (!ctx.uses_temp_registers)
        return;
    auto &os = ctx.os;
    // DXBC-to-DXIL linearizes each temporary register component into the
    // second operand of TempRegLoad/TempRegStore.  Keep separate typed
    // per-invocation arrays so integer bit patterns and floating values are
    // not coerced through a common type.
    os << "  thread bool m12_temp_bool[4096] = {};\n";
    os << "  thread int m12_temp_int[4096] = {};\n";
    os << "  thread uint m12_temp_uint[4096] = {};\n";
    os << "  thread float m12_temp_float[4096] = {};\n";
    os << "  thread long m12_temp_long[4096] = {};\n";
    os << "  thread ulong m12_temp_ulong[4096] = {};\n";
}

static void emitFunctionPrologue(LowerContext &ctx) {
    auto &os = ctx.os;
    if (ctx.uses_atomic64_emulation)
        ctx.binding_plan.direct_buffer_count =
            std::min<uint32_t>(ctx.binding_plan.direct_buffer_count, 28);
    if (ctx.options.vrs_per_primitive && ctx.shader.kind == DxilShaderKind::Pixel)
        ctx.binding_plan.direct_buffer_count =
            std::min<uint32_t>(ctx.binding_plan.direct_buffer_count, 27);
    if (ctx.options.conservative_rasterization &&
        ctx.shader.kind == DxilShaderKind::Pixel)
        ctx.binding_plan.direct_buffer_count =
            std::min<uint32_t>(ctx.binding_plan.direct_buffer_count, 26);
    os << kMetalHeader;
    emitBindingManifest(ctx);

    std::map<std::pair<uint32_t, uint32_t>, uint32_t>
        dynamic_buffer_ranges;
    for (const auto &entry : ctx.resource_handles) {
        const auto &handle = entry.second;
        if (handle.kind != DescriptorRangePlan::Kind::SRV ||
            handle.dynamic_index.empty() || handle.binding_count <= 1)
            continue;
        const uint32_t base = directBufferBindingIndex(ctx, handle, "buf");
        if (base >= ctx.binding_plan.direct_buffer_count)
            continue;
        const uint32_t count = std::min<uint32_t>(
            handle.binding_count, ctx.binding_plan.direct_buffer_count - base);
        if (count > 1)
            dynamic_buffer_ranges[{base, count}] =
                handle.direct_heap ? handle.lower_bound : 0u;
    }
    for (const auto &[range, index_base] : dynamic_buffer_ranges) {
        const uint32_t base = range.first;
        const uint32_t count = range.second;
        const std::string helper = "m12_dynamic_buffer_load_" +
                                   std::to_string(base) + "_" +
                                   std::to_string(count);
        os << "static inline uint4 " << helper
           << "(uint index, int byte_offset";
        for (uint32_t i = 0; i < count; ++i)
            os << ", device char* b" << (base + i);
        os << ") {\n";
        for (uint32_t i = 0; i < count; ++i)
            os << "  if (index == " << (index_base + i)
               << "u) return reinterpret_cast<device uint4&>(b"
               << (base + i) << "[byte_offset]);\n";
        os << "  return uint4(0);\n";
        os << "}\n\n";
    }

    if (ctx.uses_double_emulation) {
        os << "static inline ulong m12_f64_shift_right_sticky(ulong value, uint shift) {\n";
        os << "  if (shift == 0u) return value;\n";
        os << "  if (shift >= 64u) return value == 0ul ? 0ul : 1ul;\n";
        os << "  ulong mask = (1ul << shift) - 1ul;\n";
        os << "  return (value >> shift) | ((value & mask) != 0ul ? 1ul : 0ul);\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_add(ulong a, ulong b) {\n";
        os << "  const ulong frac_mask = 0x000ffffffffffffful;\n";
        os << "  const ulong quiet_nan = 0x7ff8000000000000ul;\n";
        os << "  uint ea = uint((a >> 52) & 0x7fful);\n";
        os << "  uint eb = uint((b >> 52) & 0x7fful);\n";
        os << "  ulong fa = a & frac_mask;\n";
        os << "  ulong fb = b & frac_mask;\n";
        os << "  bool sign_a = (a >> 63) != 0ul;\n";
        os << "  bool sign_b = (b >> 63) != 0ul;\n";
        os << "  if (ea == 0x7ffu) {\n";
        os << "    if (fa != 0ul || (eb == 0x7ffu && fb == 0ul && sign_a != sign_b)) return quiet_nan;\n";
        os << "    return a;\n";
        os << "  }\n";
        os << "  if (eb == 0x7ffu) return fb != 0ul ? quiet_nan : b;\n";
        os << "  if (ea == 0u && fa == 0ul) return (eb == 0u && fb == 0ul) ? ((sign_a && sign_b) ? (1ul << 63) : 0ul) : b;\n";
        os << "  if (eb == 0u && fb == 0ul) return a;\n";
        os << "  uint xa = ea == 0u ? 1u : ea;\n";
        os << "  uint xb = eb == 0u ? 1u : eb;\n";
        os << "  ulong sa = (fa | (ea == 0u ? 0ul : (1ul << 52))) << 3;\n";
        os << "  ulong sb = (fb | (eb == 0u ? 0ul : (1ul << 52))) << 3;\n";
        os << "  if (xa < xb || (xa == xb && sa < sb)) {\n";
        os << "    uint tx = xa; xa = xb; xb = tx;\n";
        os << "    ulong ts = sa; sa = sb; sb = ts;\n";
        os << "    bool tsgn = sign_a; sign_a = sign_b; sign_b = tsgn;\n";
        os << "  }\n";
        os << "  sb = m12_f64_shift_right_sticky(sb, xa - xb);\n";
        os << "  ulong sig = sign_a == sign_b ? sa + sb : sa - sb;\n";
        os << "  if (sig == 0ul) return 0ul;\n";
        os << "  uint exponent = xa;\n";
        os << "  if (sign_a == sign_b && (sig & (1ul << 56)) != 0ul) {\n";
        os << "    sig = m12_f64_shift_right_sticky(sig, 1u);\n";
        os << "    exponent += 1u;\n";
        os << "  } else {\n";
        os << "    while (exponent > 1u && (sig & (1ul << 55)) == 0ul) { sig <<= 1; exponent -= 1u; }\n";
        os << "  }\n";
        os << "  ulong main = sig >> 3;\n";
        os << "  ulong tail = sig & 7ul;\n";
        os << "  if (tail > 4ul || (tail == 4ul && (main & 1ul) != 0ul)) main += 1ul;\n";
        os << "  if ((main & (1ul << 53)) != 0ul) { main >>= 1; exponent += 1u; }\n";
        os << "  if (exponent >= 0x7ffu) return (sign_a ? (1ul << 63) : 0ul) | (0x7fful << 52);\n";
        os << "  ulong encoded_exponent = exponent == 1u && (main & (1ul << 52)) == 0ul ? 0ul : ulong(exponent);\n";
        os << "  return (sign_a ? (1ul << 63) : 0ul) | (encoded_exponent << 52) | (main & frac_mask);\n";
        os << "}\n\n";
        os << "struct m12_f64_parts { bool sign; int exponent; ulong significand; uint special; };\n";
        os << "static inline m12_f64_parts m12_f64_unpack(ulong bits) {\n";
        os << "  m12_f64_parts p;\n";
        os << "  p.sign = (bits >> 63) != 0ul;\n";
        os << "  uint exponent = uint((bits >> 52) & 0x7fful);\n";
        os << "  ulong fraction = bits & 0x000ffffffffffffful;\n";
        os << "  p.special = 0u;\n";
        os << "  if (exponent == 0x7ffu) { p.special = fraction == 0ul ? 2u : 3u; p.exponent = 0; p.significand = fraction; return p; }\n";
        os << "  if (exponent == 0u) {\n";
        os << "    if (fraction == 0ul) { p.special = 1u; p.exponent = -1022; p.significand = 0ul; return p; }\n";
        os << "    int shift = 0;\n";
        os << "    while ((fraction & (1ul << 52)) == 0ul) { fraction <<= 1; shift += 1; }\n";
        os << "    p.exponent = -1022 - shift;\n";
        os << "    p.significand = fraction;\n";
        os << "    return p;\n";
        os << "  }\n";
        os << "  p.exponent = int(exponent) - 1023;\n";
        os << "  p.significand = fraction | (1ul << 52);\n";
        os << "  return p;\n";
        os << "}\n\n";
        os << "static inline bool m12_f64_any_low128(ulong lo, ulong hi, uint count) {\n";
        os << "  if (count == 0u) return false;\n";
        os << "  if (count < 64u) return (lo & ((1ul << count) - 1ul)) != 0ul;\n";
        os << "  if (count == 64u) return lo != 0ul;\n";
        os << "  if (count < 128u) return lo != 0ul || (hi & ((1ul << (count - 64u)) - 1ul)) != 0ul;\n";
        os << "  return lo != 0ul || hi != 0ul;\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_bit128(ulong lo, ulong hi, uint index) {\n";
        os << "  if (index < 64u) return (lo >> index) & 1ul;\n";
        os << "  if (index < 128u) return (hi >> (index - 64u)) & 1ul;\n";
        os << "  return 0ul;\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_round128(ulong lo, ulong hi, uint shift, bool extra_sticky) {\n";
        os << "  ulong main = 0ul;\n";
        os << "  if (shift == 0u) main = lo;\n";
        os << "  else if (shift < 64u) main = (hi << (64u - shift)) | (lo >> shift);\n";
        os << "  else if (shift == 64u) main = hi;\n";
        os << "  else if (shift < 128u) main = hi >> (shift - 64u);\n";
        os << "  bool guard = shift != 0u && m12_f64_bit128(lo, hi, shift - 1u) != 0ul;\n";
        os << "  bool sticky = extra_sticky || (shift > 1u && m12_f64_any_low128(lo, hi, shift - 1u));\n";
        os << "  if (guard && (sticky || (main & 1ul) != 0ul)) main += 1ul;\n";
        os << "  return main;\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_pack_rounded(bool sign, int exponent, ulong lo, ulong hi, uint shift, bool extra_sticky) {\n";
        os << "  if (exponent < -1022) {\n";
        os << "    uint extra_shift = uint(-1022 - exponent);\n";
        os << "    shift = shift > 128u - min(extra_shift, 128u) ? 128u : shift + extra_shift;\n";
        os << "    ulong subnormal = m12_f64_round128(lo, hi, shift, extra_sticky);\n";
        os << "    if (subnormal >= (1ul << 52)) return sign ? (1ul << 63) | (1ul << 52) : (1ul << 52);\n";
        os << "    return (sign ? (1ul << 63) : 0ul) | subnormal;\n";
        os << "  }\n";
        os << "  ulong main = m12_f64_round128(lo, hi, shift, extra_sticky);\n";
        os << "  if (main >= (1ul << 53)) { main >>= 1; exponent += 1; }\n";
        os << "  if (exponent >= 1024) return (sign ? (1ul << 63) : 0ul) | (0x7fful << 52);\n";
        os << "  return (sign ? (1ul << 63) : 0ul) | (ulong(exponent + 1023) << 52) | (main & 0x000ffffffffffffful);\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_sqrt(ulong bits) {\n";
        os << "  m12_f64_parts p = m12_f64_unpack(bits);\n";
        os << "  if (p.special == 3u) return bits | 0x0008000000000000ul;\n";
        os << "  if (p.special == 2u) return p.sign ? 0x7ff8000000000000ul : bits;\n";
        os << "  if (p.special == 1u) return bits;\n";
        os << "  if (p.sign) return 0x7ff8000000000000ul;\n";
        os << "  int exponent = p.exponent;\n";
        os << "  ulong significand = p.significand;\n";
        os << "  if ((exponent & 1) != 0) { significand <<= 1; exponent -= 1; }\n";
        os << "  ulong root = 0ul;\n";
        os << "  ulong remainder = 0ul;\n";
        os << "  bool top_odd = (significand & (1ul << 53)) != 0ul;\n";
        os << "  uint first_digit = top_odd\n";
        os << "      ? (uint(((significand >> 53) & 1ul) << 1) | uint((significand >> 52) & 1ul))\n";
        os << "      : uint((significand >> 52) & 1ul);\n";
        os << "  remainder = ulong(first_digit);\n";
        os << "  root = 0ul;\n";
        os << "  ulong trial = 1ul;\n";
        os << "  if (remainder >= trial) { remainder -= trial; root = 1ul; }\n";
        os << "  for (int high = 103; high >= 0; high -= 2) {\n";
        os << "    uint digit = 0u;\n";
        os << "    if (high >= 52) digit |= uint((significand >> uint(high - 52)) & 1ul) << 1;\n";
        os << "    if (high - 1 >= 52) digit |= uint((significand >> uint(high - 53)) & 1ul);\n";
        os << "    remainder = (remainder << 2) | ulong(digit);\n";
        os << "    root <<= 1;\n";
        os << "    trial = (root << 1) | 1ul;\n";
        os << "    if (remainder >= trial) { remainder -= trial; root += 1ul; }\n";
        os << "  }\n";
        os << "  if (remainder > root) root += 1ul;\n";
        os << "  if (root >= (1ul << 53)) { root >>= 1; exponent += 1; }\n";
        os << "  return m12_f64_pack_rounded(false, exponent / 2, root, 0ul, 0u, false);\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_trunc(ulong bits) {\n";
        os << "  m12_f64_parts p = m12_f64_unpack(bits);\n";
        os << "  if (p.special != 0u || p.exponent >= 52) return bits;\n";
        os << "  if (p.exponent < 0) return p.sign ? (1ul << 63) : 0ul;\n";
        os << "  uint fractional_bits = uint(52 - p.exponent);\n";
        os << "  ulong mask = (1ul << fractional_bits) - 1ul;\n";
        os << "  return bits & ~(mask);\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_floor(ulong bits) {\n";
        os << "  m12_f64_parts p = m12_f64_unpack(bits);\n";
        os << "  if (p.special != 0u || p.exponent >= 52) return bits;\n";
        os << "  ulong truncated = m12_f64_trunc(bits);\n";
        os << "  if (!p.sign && truncated == bits) return bits;\n";
        os << "  if (p.sign && truncated != bits) return m12_f64_add(truncated, 0xbff0000000000000ul);\n";
        os << "  return truncated;\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_ceil(ulong bits) {\n";
        os << "  m12_f64_parts p = m12_f64_unpack(bits);\n";
        os << "  if (p.special != 0u || p.exponent >= 52) return bits;\n";
        os << "  ulong truncated = m12_f64_trunc(bits);\n";
        os << "  if (truncated == bits) return bits;\n";
        os << "  if (p.sign) return truncated;\n";
        os << "  return m12_f64_add(truncated, 0x3ff0000000000000ul);\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_round_ne(ulong bits) {\n";
        os << "  m12_f64_parts p = m12_f64_unpack(bits);\n";
        os << "  if (p.special != 0u || p.exponent >= 52) return bits;\n";
        os << "  ulong truncated = m12_f64_trunc(bits);\n";
        os << "  bool increment = false;\n";
        os << "  if (p.exponent == -1) increment = p.significand > (1ul << 52);\n";
        os << "  else if (p.exponent >= 0) {\n";
        os << "    uint fractional_bits = uint(52 - p.exponent);\n";
        os << "    ulong fraction_mask = (1ul << fractional_bits) - 1ul;\n";
        os << "    ulong fraction = p.significand & fraction_mask;\n";
        os << "    ulong halfway = 1ul << (fractional_bits - 1u);\n";
        os << "    increment = fraction > halfway || (fraction == halfway && ((p.significand >> fractional_bits) & 1ul) != 0ul);\n";
        os << "  }\n";
        os << "  return increment ? m12_f64_add(truncated, p.sign ? 0xbff0000000000000ul : 0x3ff0000000000000ul) : truncated;\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_frac(ulong bits) {\n";
        os << "  m12_f64_parts p = m12_f64_unpack(bits);\n";
        os << "  if (p.special == 3u) return bits | 0x0008000000000000ul;\n";
        os << "  if (p.special == 2u) return 0x7ff8000000000000ul;\n";
        os << "  if (p.special == 1u) return 0ul;\n";
        os << "  return m12_f64_add(bits, m12_f64_floor(bits) ^ (1ul << 63));\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_mul(ulong a, ulong b) {\n";
        os << "  m12_f64_parts pa = m12_f64_unpack(a);\n";
        os << "  m12_f64_parts pb = m12_f64_unpack(b);\n";
        os << "  bool sign = pa.sign != pb.sign;\n";
        os << "  if (pa.special == 3u || pb.special == 3u || (pa.special == 2u && pb.special == 1u) || (pa.special == 1u && pb.special == 2u)) return 0x7ff8000000000000ul;\n";
        os << "  if (pa.special == 2u || pb.special == 2u) return (sign ? (1ul << 63) : 0ul) | (0x7fful << 52);\n";
        os << "  if (pa.special == 1u || pb.special == 1u) return sign ? (1ul << 63) : 0ul;\n";
        os << "  ulong a0 = pa.significand & 0xfffffffful;\n";
        os << "  ulong a1 = pa.significand >> 32;\n";
        os << "  ulong b0 = pb.significand & 0xfffffffful;\n";
        os << "  ulong b1 = pb.significand >> 32;\n";
        os << "  ulong p0 = a0 * b0;\n";
        os << "  ulong p1 = a0 * b1 + a1 * b0;\n";
        os << "  ulong p2 = a1 * b1;\n";
        os << "  ulong lo = p0 + (p1 << 32);\n";
        os << "  ulong hi = p2 + (p1 >> 32) + (lo < p0 ? 1ul : 0ul);\n";
        os << "  bool top = (hi & (1ul << 41)) != 0ul;\n";
        os << "  int exponent = pa.exponent + pb.exponent + (top ? 1 : 0);\n";
        os << "  return m12_f64_pack_rounded(sign, exponent, lo, hi, top ? 53u : 52u, false);\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_div_quotient(ulong numerator, ulong denominator, thread ulong &remainder) {\n";
        os << "  ulong quotient = 0ul;\n";
        os << "  ulong rem = 0ul;\n";
        os << "  for (int bit = 52; bit >= 0; --bit) {\n";
        os << "    rem = (rem << 1) | ((numerator >> uint(bit)) & 1ul);\n";
        os << "    if (rem >= denominator) { rem -= denominator; quotient = (quotient << 1) | 1ul; } else quotient <<= 1;\n";
        os << "  }\n";
        os << "  for (uint bit = 0u; bit < 55u; ++bit) {\n";
        os << "    rem <<= 1;\n";
        os << "    if (rem >= denominator) { rem -= denominator; quotient = (quotient << 1) | 1ul; } else quotient <<= 1;\n";
        os << "  }\n";
        os << "  remainder = rem;\n";
        os << "  return quotient;\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_div(ulong a, ulong b) {\n";
        os << "  m12_f64_parts pa = m12_f64_unpack(a);\n";
        os << "  m12_f64_parts pb = m12_f64_unpack(b);\n";
        os << "  bool sign = pa.sign != pb.sign;\n";
        os << "  if (pa.special == 3u || pb.special == 3u || (pa.special == 2u && pb.special == 2u) || (pa.special == 1u && pb.special == 1u)) return 0x7ff8000000000000ul;\n";
        os << "  if (pa.special == 2u) return (sign ? (1ul << 63) : 0ul) | (0x7fful << 52);\n";
        os << "  if (pb.special == 2u) return sign ? (1ul << 63) : 0ul;\n";
        os << "  if (pb.special == 1u) return (sign ? (1ul << 63) : 0ul) | (0x7fful << 52);\n";
        os << "  if (pa.special == 1u) return sign ? (1ul << 63) : 0ul;\n";
        os << "  thread ulong remainder = 0ul;\n";
        os << "  ulong quotient = m12_f64_div_quotient(pa.significand, pb.significand, remainder);\n";
        os << "  bool ratio_ge_one = pa.significand >= pb.significand;\n";
        os << "  int exponent = pa.exponent - pb.exponent - (ratio_ge_one ? 0 : 1);\n";
        os << "  return m12_f64_pack_rounded(sign, exponent, quotient, 0ul, ratio_ge_one ? 3u : 2u, remainder != 0ul);\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_pack_scaled(bool sign, ulong significand, int scale) {\n";
        os << "  if (significand == 0ul) return sign ? (1ul << 63) : 0ul;\n";
        os << "  uint high_bit = 0u;\n";
        os << "  ulong scan = significand;\n";
        os << "  while (scan > 1ul) { scan >>= 1; high_bit += 1u; }\n";
        os << "  int exponent = scale + int(high_bit);\n";
        os << "  if (exponent >= -1022) {\n";
        os << "    ulong main = significand << (52u - high_bit);\n";
        os << "    return (sign ? (1ul << 63) : 0ul) | (ulong(exponent + 1023) << 52) | (main & 0x000ffffffffffffful);\n";
        os << "  }\n";
        os << "  int sub_shift = -(scale + 1074);\n";
        os << "  ulong subnormal = sub_shift > 0 ? m12_f64_round128(significand, 0ul, uint(sub_shift), false) : significand << uint(-sub_shift);\n";
        os << "  if (subnormal >= (1ul << 52)) return (sign ? (1ul << 63) : 0ul) | (1ul << 52);\n";
        os << "  return (sign ? (1ul << 63) : 0ul) | subnormal;\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_from_float(float value) {\n";
        os << "  uint bits = as_type<uint>(value);\n";
        os << "  bool sign = (bits >> 31) != 0u;\n";
        os << "  uint exponent = (bits >> 23) & 0xffu;\n";
        os << "  ulong fraction = ulong(bits & 0x7fffffu);\n";
        os << "  if (exponent == 0xffu) return (sign ? (1ul << 63) : 0ul) | (0x7fful << 52) | (fraction != 0ul ? 0x0008000000000000ul : 0ul) | (fraction << 29);\n";
        os << "  if (exponent == 0u) return m12_f64_pack_scaled(sign, fraction, -149);\n";
        os << "  int unbiased = int(exponent) - 127;\n";
        os << "  ulong significand = (fraction | (1ul << 23)) << 29;\n";
        os << "  return (sign ? (1ul << 63) : 0ul) | (ulong(unbiased + 1023) << 52) | (significand & 0x000ffffffffffffful);\n";
        os << "}\n\n";
        os << "static inline float m12_f64_to_float(ulong bits) {\n";
        os << "  m12_f64_parts p = m12_f64_unpack(bits);\n";
        os << "  if (p.special == 1u) return as_type<float>(p.sign ? 0x80000000u : 0u);\n";
        os << "  if (p.special == 2u) return as_type<float>((p.sign ? 0x80000000u : 0u) | 0x7f800000u);\n";
        os << "  if (p.special == 3u) return as_type<float>((p.sign ? 0x80000000u : 0u) | 0x7fc00000u);\n";
        os << "  int exponent = p.exponent;\n";
        os << "  ulong significand;\n";
        os << "  if (exponent >= -126) {\n";
        os << "    significand = m12_f64_round128(p.significand, 0ul, 29u, false);\n";
        os << "    if (significand >= (1ul << 24)) { significand >>= 1; exponent += 1; }\n";
        os << "    if (exponent > 127) return as_type<float>((p.sign ? 0x80000000u : 0u) | 0x7f800000u);\n";
        os << "    return as_type<float>((p.sign ? 0x80000000u : 0u) | (uint(exponent + 127) << 23) | (uint(significand) & 0x7fffffu));\n";
        os << "  }\n";
        os << "  int shift = -(exponent + 97);\n";
        os << "  ulong subnormal = shift > 0 ? m12_f64_round128(p.significand, 0ul, uint(shift), false) : p.significand << uint(-shift);\n";
        os << "  if (subnormal >= (1ul << 23)) return as_type<float>((p.sign ? 0x80000000u : 0u) | 0x00800000u);\n";
        os << "  return as_type<float>((p.sign ? 0x80000000u : 0u) | uint(subnormal));\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_from_uint(ulong value) {\n";
        os << "  if (value == 0ul) return 0ul;\n";
        os << "  uint high_bit = 0u; ulong scan = value;\n";
        os << "  while (scan > 1ul) { scan >>= 1; high_bit += 1u; }\n";
        os << "  ulong significand = high_bit > 52u ? m12_f64_round128(value, 0ul, high_bit - 52u, false) : value << (52u - high_bit);\n";
        os << "  int exponent = int(high_bit);\n";
        os << "  if (significand >= (1ul << 53)) { significand >>= 1; exponent += 1; }\n";
        os << "  return (ulong(exponent + 1023) << 52) | (significand & 0x000ffffffffffffful);\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_from_sint(long value) {\n";
        os << "  bool sign = value < 0;\n";
        os << "  ulong magnitude = sign ? (~ulong(value) + 1ul) : ulong(value);\n";
        os << "  return m12_f64_from_uint(magnitude) | (sign ? (1ul << 63) : 0ul);\n";
        os << "}\n\n";
        os << "static inline long m12_f64_to_slong(ulong bits) {\n";
        os << "  m12_f64_parts p = m12_f64_unpack(bits);\n";
        os << "  if (p.special != 0u || p.exponent < 0) return 0;\n";
        os << "  ulong magnitude = p.exponent >= 52 ? p.significand << uint(p.exponent - 52) : p.significand >> uint(52 - p.exponent);\n";
        os << "  return p.sign ? -long(magnitude) : long(magnitude);\n";
        os << "}\n\n";
        os << "static inline ulong m12_f64_to_ulong(ulong bits) {\n";
        os << "  m12_f64_parts p = m12_f64_unpack(bits);\n";
        os << "  if (p.special != 0u || p.sign || p.exponent < 0) return 0ul;\n";
        os << "  return p.exponent >= 52 ? p.significand << uint(p.exponent - 52) : p.significand >> uint(52 - p.exponent);\n";
        os << "}\n\n";
        os << "static inline int m12_f64_to_sint(ulong bits) { return int(m12_f64_to_slong(bits)); }\n\n";
        os << "static inline uint m12_f64_to_uint(ulong bits) { return uint(m12_f64_to_ulong(bits)); }\n\n";
        os << "static inline ulong m12_f64_remainder(ulong a, ulong b) {\n";
        os << "  m12_f64_parts pa = m12_f64_unpack(a);\n";
        os << "  m12_f64_parts pb = m12_f64_unpack(b);\n";
        os << "  if (pa.special == 3u || pb.special == 3u || pa.special == 2u || pb.special == 1u) return 0x7ff8000000000000ul;\n";
        os << "  if (pa.special == 1u) return a;\n";
        os << "  if (pa.exponent < pb.exponent || (pa.exponent == pb.exponent && pa.significand < pb.significand)) return a;\n";
        os << "  ulong remainder = 0ul;\n";
        os << "  for (int bit = 52; bit >= 0; --bit) {\n";
        os << "    remainder = (remainder << 1) | ((pa.significand >> uint(bit)) & 1ul);\n";
        os << "    if (remainder >= pb.significand) remainder -= pb.significand;\n";
        os << "  }\n";
        os << "  uint distance = uint(pa.exponent - pb.exponent);\n";
        os << "  for (uint bit = 0u; bit < distance; ++bit) {\n";
        os << "    remainder <<= 1;\n";
        os << "    if (remainder >= pb.significand) remainder -= pb.significand;\n";
        os << "  }\n";
        os << "  return m12_f64_pack_scaled(pa.sign, remainder, pb.exponent - 52);\n";
        os << "}\n\n";
        os << "static inline bool m12_f64_cmp(ulong a, ulong b, uint predicate) {\n";
        os << "  m12_f64_parts pa = m12_f64_unpack(a);\n";
        os << "  m12_f64_parts pb = m12_f64_unpack(b);\n";
        os << "  bool unordered = pa.special == 3u || pb.special == 3u;\n";
        os << "  int comparison = 0;\n";
        os << "  if (!unordered) {\n";
        os << "    ulong ma = a & 0x7ffffffffffffffful;\n";
        os << "    ulong mb = b & 0x7ffffffffffffffful;\n";
        os << "    if (ma == 0ul && mb == 0ul) comparison = 0;\n";
        os << "    else if (pa.sign != pb.sign) comparison = pa.sign ? -1 : 1;\n";
        os << "    else if (ma == mb) comparison = 0;\n";
        os << "    else comparison = (ma < mb ? -1 : 1) * (pa.sign ? -1 : 1);\n";
        os << "  }\n";
        os << "  switch (predicate) {\n";
        os << "  case 0u: return false; case 1u: return !unordered && comparison == 0;\n";
        os << "  case 2u: return !unordered && comparison > 0; case 3u: return !unordered && comparison >= 0;\n";
        os << "  case 4u: return !unordered && comparison < 0; case 5u: return !unordered && comparison <= 0;\n";
        os << "  case 6u: return !unordered && comparison != 0; case 7u: return !unordered;\n";
        os << "  case 8u: return unordered; case 9u: return unordered || comparison == 0;\n";
        os << "  case 10u: return unordered || comparison > 0; case 11u: return unordered || comparison >= 0;\n";
        os << "  case 12u: return unordered || comparison < 0; case 13u: return unordered || comparison <= 0;\n";
        os << "  case 14u: return unordered || comparison != 0; case 15u: return true;\n";
        os << "  default: return false;\n";
        os << "  }\n";
        os << "}\n\n";
    }

    if (ctx.compute_wave_shader) {
        os << "static inline uint m12_wave_match(uint value, uint lane, uint count) {\n";
        os << "  uint mask = 0u;\n";
        os << "  for (uint other = 0u; other < count; ++other)\n";
        os << "    if (value == simd_broadcast(value, other)) mask |= 1u << other;\n";
        os << "  return mask;\n";
        os << "}\n\n";
        os << "static inline uint m12_wave_multi_prefix_bit_count(uint value, uint mask, uint lane) {\n";
        os << "  uint active = static_cast<uint>(static_cast<simd_vote::vote_t>(simd_ballot(value != 0u)));\n";
        os << "  uint lower = lane == 0u ? 0u : ((1u << lane) - 1u);\n";
        os << "  return popcount(mask & active & lower);\n";
        os << "}\n\n";
        auto emitWavePrefixHelper = [&](const char *name, const char *initial,
                                        const char *operation) {
            os << "template <typename T> static inline T " << name
               << "(T value, uint mask, uint lane) {\n";
            os << "  T result = " << initial << ";\n";
            for (unsigned other = 0; other < 32; ++other) {
                os << "  T other_value_" << other << " = simd_broadcast(value, "
                   << other << "u);\n";
                os << "  if (lane > " << other << "u && (mask & (1u << "
                   << other << "u)) != 0u) result " << operation
                   << " other_value_" << other << ";\n";
            }
            os << "  return result;\n";
            os << "}\n\n";
        };
        emitWavePrefixHelper("m12_wave_multi_prefix_sum", "T(0)", "+=");
        emitWavePrefixHelper("m12_wave_multi_prefix_product", "T(1)", "*=");
        emitWavePrefixHelper("m12_wave_multi_prefix_bit_and", "T(~0u)", "&=");
        emitWavePrefixHelper("m12_wave_multi_prefix_bit_or", "T(0)", "|=");
        emitWavePrefixHelper("m12_wave_multi_prefix_bit_xor", "T(0)", "^=");
        os << "static inline long m12_wave_active_sum_long(long value) {\n";
        os << "  ulong result = 0ul;\n";
        for (unsigned other = 0; other < 32; ++other) {
            os << "  uint low_" << other << " = simd_broadcast((uint)value, " << other << "u);\n";
            os << "  uint high_" << other << " = simd_broadcast((uint)((ulong)value >> 32), " << other << "u);\n";
            os << "  result += (ulong)low_" << other << " | ((ulong)high_" << other << " << 32);\n";
        }
        os << "  return (long)result;\n";
        os << "}\n\n";
        os << "static inline long m12_wave_prefix_sum_long(long value, uint lane) {\n";
        os << "  ulong result = 0ul;\n";
        for (unsigned other = 0; other < 32; ++other) {
            os << "  uint low_" << other << " = simd_broadcast((uint)value, " << other << "u);\n";
            os << "  uint high_" << other << " = simd_broadcast((uint)((ulong)value >> 32), " << other << "u);\n";
            os << "  if (lane > " << other << "u) result += (ulong)low_" << other << " | ((ulong)high_" << other << " << 32);\n";
        }
        os << "  return (long)result;\n";
        os << "}\n\n";
        os << "static inline long m12_wave_active_product_long(long value) {\n";
        os << "  ulong result = 1ul;\n";
        for (unsigned other = 0; other < 32; ++other) {
            os << "  uint low_" << other << " = simd_broadcast((uint)value, " << other << "u);\n";
            os << "  uint high_" << other << " = simd_broadcast((uint)((ulong)value >> 32), " << other << "u);\n";
            os << "  result *= (ulong)low_" << other << " | ((ulong)high_" << other << " << 32);\n";
        }
        os << "  return (long)result;\n";
        os << "}\n\n";
        os << "static inline long m12_wave_prefix_product_long(long value, uint lane) {\n";
        os << "  ulong result = 1ul;\n";
        for (unsigned other = 0; other < 32; ++other) {
            os << "  uint low_" << other << " = simd_broadcast((uint)value, " << other << "u);\n";
            os << "  uint high_" << other << " = simd_broadcast((uint)((ulong)value >> 32), " << other << "u);\n";
            os << "  if (lane > " << other << "u) result *= (ulong)low_" << other << " | ((ulong)high_" << other << " << 32);\n";
        }
        os << "  return (long)result;\n";
        os << "}\n\n";
        os << "static inline long m12_wave_active_bit_long(long value, uint op) {\n";
        os << "  ulong result = op == 0u ? ~0ul : 0ul;\n";
        for (unsigned other = 0; other < 32; ++other) {
            os << "  uint low_" << other << " = simd_broadcast((uint)value, " << other << "u);\n";
            os << "  uint high_" << other << " = simd_broadcast((uint)((ulong)value >> 32), " << other << "u);\n";
            os << "  ulong other_value_" << other << " = (ulong)low_" << other << " | ((ulong)high_" << other << " << 32);\n";
            os << "  if (op == 0u) result &= other_value_" << other << ";\n";
            os << "  else if (op == 1u) result |= other_value_" << other << ";\n";
            os << "  else if (op == 2u) result ^= other_value_" << other << ";\n";
        }
        os << "  return (long)result;\n";
        os << "}\n\n";
        os << "static inline long m12_wave_active_extreme_long(long value, uint signed_op, bool want_min) {\n";
        os << "  ulong result = 0ul;\n";
        os << "  bool have_result = false;\n";
        for (unsigned other = 0; other < 32; ++other) {
            os << "  uint low_" << other << " = simd_broadcast((uint)value, " << other << "u);\n";
            os << "  uint high_" << other << " = simd_broadcast((uint)((ulong)value >> 32), " << other << "u);\n";
            os << "  ulong candidate_" << other << " = (ulong)low_" << other << " | ((ulong)high_" << other << " << 32);\n";
            os << "  if (!have_result || (want_min ? (signed_op == 0u ? (long)candidate_" << other << " < (long)result : candidate_" << other << " < result) : (signed_op == 0u ? (long)candidate_" << other << " > (long)result : candidate_" << other << " > result))) {\n";
            os << "    result = candidate_" << other << ";\n";
            os << "    have_result = true;\n";
            os << "  }\n";
        }
        os << "  return (long)result;\n";
        os << "}\n\n";
        os << "static inline long m12_wave_prefix_bit_long(long value, uint lane, uint op) {\n";
        os << "  ulong result = op == 0u ? ~0ul : 0ul;\n";
        for (unsigned other = 0; other < 32; ++other) {
            os << "  uint low_" << other << " = simd_broadcast((uint)value, " << other << "u);\n";
            os << "  uint high_" << other << " = simd_broadcast((uint)((ulong)value >> 32), " << other << "u);\n";
            os << "  ulong other_value_" << other << " = (ulong)low_" << other << " | ((ulong)high_" << other << " << 32);\n";
            os << "  if (lane > " << other << "u) {\n";
            os << "    if (op == 0u) result &= other_value_" << other << ";\n";
            os << "    else if (op == 1u) result |= other_value_" << other << ";\n";
            os << "    else if (op == 2u) result ^= other_value_" << other << ";\n";
            os << "  }\n";
        }
        os << "  return (long)result;\n";
        os << "}\n\n";
    }

    if (ctx.uses_atomic32_emulation) {
        os << "static inline uint m12_atomic32_binop(volatile device atomic_uint* target, uint value, uint op) {\n";
        os << "  uint expected = atomic_load_explicit(target, memory_order_relaxed);\n";
        os << "  while (true) {\n";
        os << "    uint result = expected;\n";
        os << "    switch (op) {\n";
        os << "    case 0u: result = expected + value; break;\n";
        os << "    case 1u: result = expected & value; break;\n";
        os << "    case 2u: result = expected | value; break;\n";
        os << "    case 3u: result = expected ^ value; break;\n";
        os << "    case 4u: result = uint(min(int(expected), int(value))); break;\n";
        os << "    case 5u: result = uint(max(int(expected), int(value))); break;\n";
        os << "    case 6u: result = min(expected, value); break;\n";
        os << "    case 7u: result = max(expected, value); break;\n";
        os << "    case 8u: result = value; break;\n";
        os << "    default: return expected;\n";
        os << "    }\n";
        os << "    if (atomic_compare_exchange_weak_explicit(target, &expected, result, memory_order_relaxed, memory_order_relaxed))\n";
        os << "      return expected;\n";
        os << "  }\n";
        os << "}\n\n";
        os << "static inline uint m12_atomic32_compare_exchange(volatile device atomic_uint* target, uint compare_value, uint new_value) {\n";
        os << "  uint expected = compare_value;\n";
        os << "  atomic_compare_exchange_weak_explicit(target, &expected, new_value, memory_order_relaxed, memory_order_relaxed);\n";
        os << "  return expected;\n";
        os << "}\n\n";
    }

    if (ctx.uses_atomic64_emulation) {
        os << "static inline ulong m12_atomic64_binop(volatile device ulong* target, ulong value, uint op, device atomic_uint* lock) {\n";
        os << "  bool pending = true;\n";
        os << "  ulong original = 0ul;\n";
        os << "  while (simd_any(pending)) {\n";
        os << "    bool selected = pending && simd_prefix_exclusive_sum(uint(pending)) == 0u;\n";
        os << "    if (selected) {\n";
        os << "      uint expected = 0u;\n";
        os << "      while (!atomic_compare_exchange_weak_explicit(lock, &expected, 1u, memory_order_relaxed, memory_order_relaxed)) expected = 0u;\n";
        os << "      original = *target;\n";
        os << "      ulong result = original;\n";
        os << "      switch (op) {\n";
        os << "      case 0u: result = original + value; break;\n";
        os << "      case 1u: result = original & value; break;\n";
        os << "      case 2u: result = original | value; break;\n";
        os << "      case 3u: result = original ^ value; break;\n";
        os << "      case 4u: result = ulong(min(long(original), long(value))); break;\n";
        os << "      case 5u: result = ulong(max(long(original), long(value))); break;\n";
        os << "      case 6u: result = min(original, value); break;\n";
        os << "      case 7u: result = max(original, value); break;\n";
        os << "      case 8u: result = value; break;\n";
        os << "      default: break;\n";
        os << "      }\n";
        os << "      *target = result;\n";
        os << "      atomic_store_explicit(lock, 0u, memory_order_relaxed);\n";
        os << "      pending = false;\n";
        os << "    }\n";
        os << "  }\n";
        os << "  return original;\n";
        os << "}\n\n";
        os << "static inline ulong m12_atomic64_compare_exchange(volatile device ulong* target, ulong compare_value, ulong new_value, device atomic_uint* lock) {\n";
        os << "  bool pending = true;\n";
        os << "  ulong original = 0ul;\n";
        os << "  while (simd_any(pending)) {\n";
        os << "    bool selected = pending && simd_prefix_exclusive_sum(uint(pending)) == 0u;\n";
        os << "    if (selected) {\n";
        os << "      uint expected = 0u;\n";
        os << "      while (!atomic_compare_exchange_weak_explicit(lock, &expected, 1u, memory_order_relaxed, memory_order_relaxed)) expected = 0u;\n";
        os << "      original = *target;\n";
        os << "      if (original == compare_value) *target = new_value;\n";
        os << "      atomic_store_explicit(lock, 0u, memory_order_relaxed);\n";
        os << "      pending = false;\n";
        os << "    }\n";
        os << "  }\n";
        os << "  return original;\n";
        os << "}\n\n";
        if (ctx.uses_group_atomic64_emulation) {
            os << "static inline ulong m12_atomic64_binop_group(volatile threadgroup ulong* target, ulong value, uint op, threadgroup atomic_uint* lock) {\n";
            os << "  bool pending = true;\n";
            os << "  ulong original = 0ul;\n";
            os << "  while (simd_any(pending)) {\n";
            os << "    bool selected = pending && simd_prefix_exclusive_sum(uint(pending)) == 0u;\n";
            os << "    if (selected) {\n";
            os << "      uint expected = 0u;\n";
            os << "      while (!atomic_compare_exchange_weak_explicit(lock, &expected, 1u, memory_order_relaxed, memory_order_relaxed)) expected = 0u;\n";
            os << "      original = *target;\n";
            os << "      ulong result = original;\n";
            os << "      switch (op) {\n";
            os << "      case 0u: result = original + value; break;\n";
            os << "      case 1u: result = original & value; break;\n";
            os << "      case 2u: result = original | value; break;\n";
            os << "      case 3u: result = original ^ value; break;\n";
            os << "      case 4u: result = ulong(min(long(original), long(value))); break;\n";
            os << "      case 5u: result = ulong(max(long(original), long(value))); break;\n";
            os << "      case 6u: result = min(original, value); break;\n";
            os << "      case 7u: result = max(original, value); break;\n";
            os << "      case 8u: result = value; break;\n";
            os << "      case 9u: result = original - value; break;\n";
            os << "      default: break;\n";
            os << "      }\n";
            os << "      *target = result;\n";
            os << "      atomic_store_explicit(lock, 0u, memory_order_relaxed);\n";
            os << "      pending = false;\n";
            os << "    }\n";
            os << "  }\n";
            os << "  return original;\n";
            os << "}\n\n";
            os << "static inline ulong m12_atomic64_compare_exchange_group(volatile threadgroup ulong* target, ulong compare_value, ulong new_value, threadgroup atomic_uint* lock) {\n";
            os << "  bool pending = true;\n";
            os << "  ulong original = 0ul;\n";
            os << "  while (simd_any(pending)) {\n";
            os << "    bool selected = pending && simd_prefix_exclusive_sum(uint(pending)) == 0u;\n";
            os << "    if (selected) {\n";
            os << "      uint expected = 0u;\n";
            os << "      while (!atomic_compare_exchange_weak_explicit(lock, &expected, 1u, memory_order_relaxed, memory_order_relaxed)) expected = 0u;\n";
            os << "      original = *target;\n";
            os << "      if (original == compare_value) *target = new_value;\n";
            os << "      atomic_store_explicit(lock, 0u, memory_order_relaxed);\n";
            os << "      pending = false;\n";
            os << "    }\n";
            os << "  }\n";
            os << "  return original;\n";
            os << "}\n\n";
        }
    }
    if (ctx.uses_sampler_feedback) {
        os << "static inline void m12_store_sampler_feedback(device uchar* feedback_bytes, device uint* metadata, uint kind, uint mip, uint level, uint array_slice, float2 clamped_coordinate, float2 wrapped_coordinate) {\n";
        os << "  uint level_metadata = 10u + level * 4u;\n";
        os << "  uint data_offset = metadata[level_metadata];\n";
        os << "  uint width = max(metadata[level_metadata + 1u], 1u);\n";
        os << "  uint height = max(metadata[level_metadata + 2u], 1u);\n";
        os << "  uint row_pitch = max(metadata[level_metadata + 3u], width);\n";
        os << "  uint2 position = min(uint2(clamped_coordinate * float2(width, height)), uint2(width - 1u, height - 1u));\n";
        os << "  uint byte_offset = data_offset + array_slice * row_pitch * height + position.y * row_pitch + position.x;\n";
        os << "  uchar old_value = feedback_bytes[byte_offset];\n";
        os << "  feedback_bytes[byte_offset] = kind == 0u ? uchar(min(uint(old_value), mip)) : uchar(255u);\n";
        os << "  uint2 wrapped_position = min(uint2(wrapped_coordinate * float2(width, height)), uint2(width - 1u, height - 1u));\n";
        os << "  uint wrapped_offset = data_offset + array_slice * row_pitch * height + wrapped_position.y * row_pitch + wrapped_position.x;\n";
        os << "  if (wrapped_offset != byte_offset) {\n";
        os << "    uchar wrapped_old = feedback_bytes[wrapped_offset];\n";
        os << "    feedback_bytes[wrapped_offset] = kind == 0u ? uchar(min(uint(wrapped_old), mip)) : uchar(255u);\n";
        os << "  }\n";
        os << "}\n\n";
        os << "static inline void m12_write_sampler_feedback(device char* feedback, float3 coordinate, float lod) {\n";
        os << "  device uchar* feedback_bytes = reinterpret_cast<device uchar*>(feedback);\n";
        os << "  device uint* metadata = reinterpret_cast<device uint*>(feedback);\n";
        os << "  uint kind = metadata[4];\n";
        os << "  uint array_length = max(metadata[5], 1u);\n";
        os << "  uint level_count = max(metadata[9], 1u);\n";
        os << "  uint mip = uint(clamp(floor(lod), 0.0, 254.0));\n";
        os << "  uint level = kind == 0u ? 0u : min(mip, level_count - 1u);\n";
        os << "  uint array_slice = min(uint(max(coordinate.z, 0.0)), array_length - 1u);\n";
        os << "  float2 clamped_coordinate = clamp(coordinate.xy, float2(0.0), float2(0.99999994));\n";
        os << "  float2 wrapped_coordinate = fract(coordinate.xy);\n";
        if (ctx.shader.kind == DxilShaderKind::Pixel) {
            // A spinning lock in every divergent fragment lane can prevent
            // the lock owner from making progress. Keep the SIMD group
            // converged, broadcast each active lane's request in turn, and
            // let one elected lane serialize it against other SIMD groups.
            os << "  device atomic_uint* lock = reinterpret_cast<device atomic_uint*>(feedback + 32);\n";
            os << "  uint source_lane_active = simd_is_helper_thread() ? 0u : 1u;\n";
            os << "  for (ushort source_lane = 0; source_lane < 32; ++source_lane) {\n";
            os << "    bool source_active = simd_broadcast(source_lane_active, source_lane) != 0u;\n";
            os << "    uint source_mip = simd_broadcast(mip, source_lane);\n";
            os << "    uint source_level = simd_broadcast(level, source_lane);\n";
            os << "    uint source_array_slice = simd_broadcast(array_slice, source_lane);\n";
            os << "    float2 source_clamped = simd_broadcast(clamped_coordinate, source_lane);\n";
            os << "    float2 source_wrapped = simd_broadcast(wrapped_coordinate, source_lane);\n";
            os << "    if (simd_is_first() && source_active) {\n";
            os << "      uint expected = 0u;\n";
            os << "      while (!atomic_compare_exchange_weak_explicit(lock, &expected, 1u, memory_order_relaxed, memory_order_relaxed)) expected = 0u;\n";
            os << "      m12_store_sampler_feedback(feedback_bytes, metadata, kind, source_mip, source_level, source_array_slice, source_clamped, source_wrapped);\n";
            os << "      if (kind != 0u && source_level + 1u < level_count)\n";
            os << "        m12_store_sampler_feedback(feedback_bytes, metadata, kind, source_mip, source_level + 1u, source_array_slice, source_clamped, source_wrapped);\n";
            os << "      atomic_store_explicit(lock, 0u, memory_order_relaxed);\n";
            os << "    }\n";
            os << "  }\n";
        } else {
            os << "  device atomic_uint* lock = reinterpret_cast<device atomic_uint*>(feedback + 32);\n";
            os << "  bool pending = true;\n";
            os << "  while (simd_any(pending)) {\n";
            os << "    bool selected = pending && simd_prefix_exclusive_sum(uint(pending)) == 0u;\n";
            os << "    if (selected) {\n";
            os << "      uint expected = 0u;\n";
            os << "      while (!atomic_compare_exchange_weak_explicit(lock, &expected, 1u, memory_order_relaxed, memory_order_relaxed)) expected = 0u;\n";
            os << "      m12_store_sampler_feedback(feedback_bytes, metadata, kind, mip, level, array_slice, clamped_coordinate, wrapped_coordinate);\n";
            os << "      if (kind != 0u && level + 1u < level_count)\n";
            os << "        m12_store_sampler_feedback(feedback_bytes, metadata, kind, mip, level + 1u, array_slice, clamped_coordinate, wrapped_coordinate);\n";
            os << "      atomic_store_explicit(lock, 0u, memory_order_relaxed);\n";
            os << "      pending = false;\n";
            os << "    }\n";
            os << "  }\n";
        }
        os << "}\n\n";
    }
    os << "struct input_v {\n";
    os << "  float4 position [[position]];\n";
    auto emitInputField = [&](const char *type, const char *name,
                              unsigned location) {
        os << "  ";
        if (ctx.uses_interpolation)
            os << "interpolant<" << type
               << ", interpolation::perspective> ";
        else
            os << type << " ";
        os << name << " [[user(locn" << location << ")]];\n";
    };
    emitInputField("float4", "v0", 0);
    emitInputField("float4", "v1", 1);
    emitInputField("float4", "v2", 2);
    emitInputField("float4", "v3", 3);
    emitInputField("float4", "v4", 4);
    emitInputField("float4", "v5", 5);
    emitInputField("float4", "v6", 6);
    emitInputField("float4", "v7", 7);
    emitInputField("float2", "uv0", 8);
    emitInputField("float2", "uv1", 9);
    emitInputField("float2", "uv2", 10);
    emitInputField("float2", "uv3", 11);
    emitInputField("float4", "color0", 12);
    emitInputField("float4", "color1", 13);
    emitInputField("float4", "color2", 14);
    emitInputField("float4", "color3", 15);
    if (ctx.shader.kind == DxilShaderKind::Pixel &&
        ctx.shader.shading_rate_input_register >= 0)
        os << "  uint shading_rate [[user(locn16)]];\n";
    if (ctx.shader.kind == DxilShaderKind::Pixel &&
        ctx.shader.viewport_index_input_register >= 0)
        os << "  uint viewport_array_index [[viewport_array_index]];\n";
    if (ctx.shader.kind == DxilShaderKind::Pixel &&
        ctx.shader.render_target_array_index_input_register >= 0)
        os << "  uint render_target_array_index [[render_target_array_index]];\n";
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
    if (ctx.shader.kind == DxilShaderKind::Vertex &&
        ctx.shader.shading_rate_output_register >= 0)
        os << "  uint shading_rate [[user(locn16)]];\n";
    if (ctx.shader.kind == DxilShaderKind::Vertex &&
        ctx.shader.viewport_index_output_register >= 0)
        os << "  uint viewport_array_index [[viewport_array_index]];\n";
    if (ctx.shader.kind == DxilShaderKind::Vertex &&
        ctx.shader.render_target_array_index_output_register >= 0)
        os << "  uint render_target_array_index [[render_target_array_index]];\n";
    os << "};\n\n";

    if (ctx.shader.kind == DxilShaderKind::Vertex) {
        os << "struct m12_vertex_buffer_entry { ulong buffer_handle; uint stride; uint length; };\n";
        os << "struct m12_draw_argument { uint vertexCountPerInstance; uint instanceCount; uint startVertexLocation; uint startInstanceLocation; };\n";
        os << "struct m12_draw_indexed_argument { uint indexCountPerInstance; uint instanceCount; uint startIndexLocation; int baseVertexLocation; uint startInstanceLocation; };\n";
        os << "static inline bool m12_is_indexed_draw(device char* draw_info_bytes) {\n";
        os << "  if (draw_info_bytes == nullptr) return false;\n";
        os << "  return *reinterpret_cast<device ushort*>(draw_info_bytes) != 0;\n";
        os << "}\n";
        os << "static inline uint m12_draw_vertex_count(device char* draw_bytes, device char* draw_info_bytes) {\n";
        os << "  if (draw_bytes == nullptr) return 0u;\n";
        os << "  if (m12_is_indexed_draw(draw_info_bytes)) return reinterpret_cast<device m12_draw_indexed_argument*>(draw_bytes)->indexCountPerInstance;\n";
        os << "  return reinterpret_cast<device m12_draw_argument*>(draw_bytes)->vertexCountPerInstance;\n";
        os << "}\n";
        os << "static inline uint m12_vertex_fetch_index(uint vid, device char* draw_bytes, device char* draw_info_bytes) {\n";
        os << "  if (draw_bytes == nullptr || !m12_is_indexed_draw(draw_info_bytes)) return vid;\n";
        os << "  device m12_draw_indexed_argument* draw = reinterpret_cast<device m12_draw_indexed_argument*>(draw_bytes);\n";
        os << "  int indexed_vertex = int(vid) + draw->baseVertexLocation;\n";
        os << "  return indexed_vertex < 0 ? 0u : uint(indexed_vertex);\n";
        os << "}\n";
        os << "static inline uint m12_instance_fetch_index(uint iid, uint step_rate, device char* draw_bytes, device char* draw_info_bytes) {\n";
        os << "  uint start_instance = 0;\n";
        os << "  if (draw_bytes != nullptr) {\n";
        os << "    if (m12_is_indexed_draw(draw_info_bytes)) start_instance = reinterpret_cast<device m12_draw_indexed_argument*>(draw_bytes)->startInstanceLocation;\n";
        os << "    else start_instance = reinterpret_cast<device m12_draw_argument*>(draw_bytes)->startInstanceLocation;\n";
        os << "  }\n";
        os << "  if (step_rate == 0) return start_instance;\n";
        os << "  return start_instance + (iid / step_rate);\n";
        os << "}\n";
        os << "static inline float4 m12_load_vertex_attr(uint table_index, uint aligned_byte_offset, uint dxgi_format, uint per_instance, uint step_rate, uint vid, uint iid, device char* table_bytes, device char* vb, device char* draw_bytes, device char* draw_info_bytes) {\n";
        os << "  if (table_bytes == nullptr || vb == nullptr) return float4(0.0);\n";
        os << "  device m12_vertex_buffer_entry* table = reinterpret_cast<device m12_vertex_buffer_entry*>(table_bytes);\n";
        os << "  uint stride = table[table_index].stride;\n";
        os << "  uint length = table[table_index].length;\n";
        os << "  if (stride == 0) stride = 16;\n";
        os << "  uint element_index = per_instance != 0 ? m12_instance_fetch_index(iid, step_rate, draw_bytes, draw_info_bytes) : m12_vertex_fetch_index(vid, draw_bytes, draw_info_bytes);\n";
        os << "  uint offset = element_index * stride + aligned_byte_offset;\n";
        os << "  uint required = (dxgi_format == 41 || dxgi_format == 42 || dxgi_format == 43) ? 4u : ((dxgi_format == 16 || dxgi_format == 17 || dxgi_format == 18) ? 8u : ((dxgi_format == 6 || dxgi_format == 7 || dxgi_format == 8) ? 12u : 16u));\n";
        os << "  if (length != 0 && offset + required > length) return float4(0.0);\n";
        os << "  if (dxgi_format == 41) return float4(*reinterpret_cast<device float*>(vb + offset), 0.0, 0.0, 1.0);\n";
        os << "  if (dxgi_format == 16) return float4(*reinterpret_cast<device float2*>(vb + offset), 0.0, 1.0);\n";
        os << "  if (dxgi_format == 6) return float4(*reinterpret_cast<device float3*>(vb + offset), 1.0);\n";
        os << "  if (dxgi_format == 2) return *reinterpret_cast<device float4*>(vb + offset);\n";
        os << "  if (dxgi_format == 28) return float4(*reinterpret_cast<device uchar4*>(vb + offset)) / 255.0;\n";
        os << "  if (dxgi_format == 30) return float4(*reinterpret_cast<device uchar4*>(vb + offset));\n";
        os << "  if (dxgi_format == 42) return float4(*reinterpret_cast<device uint*>(vb + offset), 0.0, 0.0, 1.0);\n";
        os << "  if (dxgi_format == 43) return float4(*reinterpret_cast<device int*>(vb + offset), 0.0, 0.0, 1.0);\n";
        os << "  return *reinterpret_cast<device float4*>(vb + offset);\n";
        os << "}\n\n";
    } else {
        os << "struct vertex_input_v {\n";
        os << "};\n\n";
    }

    if (ctx.shader.kind == DxilShaderKind::Compute) {
        os << "kernel void cs_main(\n";
        for (uint32_t i = 0; i < ctx.binding_plan.direct_buffer_count; i++) {
            if (accelerationStructureAtBufferSlot(ctx, i))
                os << "  instance_acceleration_structure as" << i
                   << " [[buffer(" << i << ")]],\n";
            else
                os << "  device char* buf" << i << " [[buffer(" << i
                   << ")]],\n";
        }
        for (uint32_t i = 0; i < ctx.binding_plan.direct_texture_count; i++) {
            bool comparison_slot = false;
            bool raw_gather_slot = false;
            bool srv_slot = false;
            bool uav_slot = false;
            if (ctx.compute_sample_cmp_shader)
                comparison_slot = ctx.comparison_texture_slots.count(i) != 0;
            if (ctx.compute_raw_gather_shader) {
                for (const auto &range : ctx.binding_plan.ranges) {
                    if (range.kind == DescriptorRangePlan::Kind::SRV &&
                        i >= range.lower_bound &&
                        i < range.lower_bound + range.count) {
                        raw_gather_slot = true;
                        break;
                    }
                }
            }
            for (const auto &range : ctx.binding_plan.ranges) {
                if (range.kind == DescriptorRangePlan::Kind::SRV &&
                    i >= range.lower_bound &&
                    i < range.lower_bound + range.count) {
                    srv_slot = true;
                }
                if (range.kind == DescriptorRangePlan::Kind::UAV &&
                    i >= range.lower_bound &&
                    i < range.lower_bound + range.count) {
                    uav_slot = true;
                }
            }
            uint32_t resource_kind = resourceKindForTextureSlot(ctx, i);
            uint32_t element_type = resourceElementTypeForTextureSlot(ctx, i);
            bool integer_element = isIntegerResourceElementType(element_type);
            bool signed_integer = isSignedResourceElementType(element_type);
            if (resource_kind != 0u) {
                bool texture_uav_slot = textureSlotHasRangeKind(
                    ctx, i, DescriptorRangePlan::Kind::UAV);
                bool writable_msaa = texture_uav_slot &&
                    (isTextureMSAAResourceKind(resource_kind) ||
                     ctx.texture_store_sample_shader ||
                     ctx.writable_msaa_texture_slots.count(i));
                if (raw_gather_slot)
                    os << "  " << textureBindingType(resource_kind, false, true, true,
                                                          false, false, true)
                       << " tex" << i << " [[texture(" << i << ")]],\n";
                else if (comparison_slot && (resource_kind == 2u ||
                                               resource_kind == 5u ||
                                               resource_kind == 7u ||
                                               resource_kind == 9u ||
                                               resource_kind == 0u))
                    os << "  " << depthTextureBindingType(resource_kind)
                       << " tex" << i << " [[texture(" << i << ")]],\n";
                else if (uav_slot && ctx.uses_sampler_feedback && srv_slot)
                    os << "  " << textureBindingType(resource_kind, false, false, true)
                       << " tex" << i << " [[texture(" << i << ")]],\n";
                else
                    os << "  " << textureBindingType(
                               resource_kind, texture_uav_slot, integer_element,
                               !texture_uav_slot && ctx.compute_texture_sample_shader,
                               writable_msaa, signed_integer)
                       << " tex" << i << " [[texture(" << i << ")]],\n";
            } else if (raw_gather_slot)
                os << "  texture2d<uint, access::sample> tex" << i << " [[texture(" << i << ")]],\n";
            else if (comparison_slot)
                os << "  depth2d<float, access::sample> tex" << i << " [[texture(" << i << ")]],\n";
            else if (uav_slot && ctx.uses_sampler_feedback && srv_slot)
                os << "  texture2d<float, access::sample> tex" << i << " [[texture(" << i << ")]],\n";
            else if (uav_slot && (ctx.texture_store_sample_shader ||
                                  ctx.writable_msaa_texture_slots.count(i)))
                os << "  texture2d_array<float, access::read_write> tex" << i << " [[texture(" << i << ")]],\n";
            else if (ctx.compute_texture_store_shader ||
                     !ctx.compute_texture_sample_shader)
                os << "  texture2d<float, access::read_write> tex" << i << " [[texture(" << i << ")]],\n";
            else
                os << "  texture2d<float, access::sample> tex" << i << " [[texture(" << i << ")]],\n";
        }
        for (uint32_t i = 0; i < ctx.binding_plan.direct_sampler_count; i++)
            os << "  sampler samp" << i << " [[sampler(" << i << ")]],\n";
        if (ctx.uses_atomic64_emulation)
            os << "  device atomic_uint* m12_atomic64_lock [[buffer(28)]],\n";
        os << "  uint3 dtid [[thread_position_in_grid]],\n";
        os << "  uint3 gtid [[thread_position_in_threadgroup]],\n";
        os << "  uint3 ggid [[threadgroup_position_in_grid]],\n";
        os << "  uint3 gsz [[threads_per_threadgroup]],\n";
        os << "  uint simd_lane [[thread_index_in_simdgroup]],\n";
        os << "  uint simd_count [[threads_per_simdgroup]]\n) {\n";
        emitTempRegisterDeclarations(ctx);
        if (ctx.uses_group_atomic64_emulation) {
            os << "  threadgroup atomic_uint m12_atomic64_group_lock;\n";
            os << "  if (all(gtid == uint3(0))) atomic_store_explicit(&m12_atomic64_group_lock, 0u, memory_order_relaxed);\n";
            os << "  threadgroup_barrier(mem_flags::mem_threadgroup);\n";
        }
    } else if (ctx.shader.kind == DxilShaderKind::Vertex) {
        os << "vertex output_v vs_main(\n";
        os << "  uint vid [[vertex_id]],\n";
        os << "  uint iid [[instance_id]],\n";
        std::vector<std::string> params;
        for (uint32_t i = 0; i < ctx.binding_plan.direct_buffer_count; i++) {
            if (accelerationStructureAtBufferSlot(ctx, i))
                params.push_back("  instance_acceleration_structure as" +
                                 std::to_string(i) + " [[buffer(" +
                                 std::to_string(i) + ")]]");
            else
                params.push_back("  device char* buf" + std::to_string(i) +
                                 " [[buffer(" + std::to_string(i) + ")]]");
        }
        for (uint32_t i = 0; i < ctx.binding_plan.direct_texture_count; i++) {
            uint32_t resource_kind = resourceKindForTextureSlot(ctx, i);
            uint32_t element_type = resourceElementTypeForTextureSlot(ctx, i);
            bool comparison_slot =
                ctx.sample_cmp_shader &&
                ctx.comparison_texture_slots.count(i) != 0;
            const bool comparison_kind =
                resource_kind == 1u || resource_kind == 2u ||
                resource_kind == 5u || resource_kind == 6u ||
                resource_kind == 7u || resource_kind == 9u ||
                resource_kind == 0u;
            const std::string binding_type = comparison_slot && comparison_kind
                ? depthTextureBindingType(resource_kind)
                : textureBindingType(
                      resource_kind, false,
                      isIntegerResourceElementType(element_type), true,
                      false, isSignedResourceElementType(element_type));
            params.push_back("  " + binding_type + " tex" +
                             std::to_string(i) + " [[texture(" +
                             std::to_string(i) + ")]]");
        }
        for (uint32_t i = 0; i < ctx.binding_plan.direct_sampler_count; i++)
            params.push_back("  sampler samp" + std::to_string(i) +
                             " [[sampler(" + std::to_string(i) + ")]]");
        for (size_t i = 0; i < params.size(); i++)
            os << params[i] << (i + 1 == params.size() ? "\n" : ",\n");
        os << ") {\n";
        os << "  output_v out = {};\n";
        emitTempRegisterDeclarations(ctx);
        emitDefaultVertexVaryingWrites(
            os, ctx.vertex_procedural_fullscreen_fallback,
            ctx.binding_plan.direct_buffer_count > 30,
            ctx.shader.shading_rate_output_register >= 0,
            ctx.shader.viewport_index_output_register >= 0,
            ctx.shader.render_target_array_index_output_register >= 0);
    } else if (ctx.shader.kind == DxilShaderKind::Pixel) {
        if (ctx.options.conservative_rasterization) {
            os << "struct m12_conservative_data { float2 p0; float2 p1; float2 p2; uint width; uint height; uint enabled; uint pad; };\n";
            os << "static inline float m12_cons_cross(float2 a, float2 b, float2 c) { return (b.x-a.x)*(c.y-a.y) - (b.y-a.y)*(c.x-a.x); }\n";
            os << "static inline bool m12_cons_point_in_tri(float2 p, float2 a, float2 b, float2 c) { float e=1.0e-5f; float s0=m12_cons_cross(a,b,p), s1=m12_cons_cross(b,c,p), s2=m12_cons_cross(c,a,p); return (s0 >= -e && s1 >= -e && s2 >= -e) || (s0 <= e && s1 <= e && s2 <= e); }\n";
            os << "static inline bool m12_cons_inner_triangle_pixel(float2 a, float2 b, float2 c, float2 lo) { float2 hi=lo+1.0f; return m12_cons_point_in_tri(lo,a,b,c) && m12_cons_point_in_tri(float2(hi.x,lo.y),a,b,c) && m12_cons_point_in_tri(hi,a,b,c) && m12_cons_point_in_tri(float2(lo.x,hi.y),a,b,c); }\n";
            os << "static inline bool m12_cons_point_in_box(float2 p, float2 lo, float2 hi) { return p.x >= lo.x-1.0e-5f && p.x <= hi.x+1.0e-5f && p.y >= lo.y-1.0e-5f && p.y <= hi.y+1.0e-5f; }\n";
            os << "static inline bool m12_cons_seg_intersects(float2 a, float2 b, float2 c, float2 d) { float e=1.0e-5f; float ab_c=m12_cons_cross(a,b,c), ab_d=m12_cons_cross(a,b,d), cd_a=m12_cons_cross(c,d,a), cd_b=m12_cons_cross(c,d,b); return ((ab_c >= -e && ab_d <= e) || (ab_c <= e && ab_d >= -e)) && ((cd_a >= -e && cd_b <= e) || (cd_a <= e && cd_b >= -e)); }\n";
            os << "static inline bool m12_cons_triangle_pixel(float2 a, float2 b, float2 c, float2 lo) { float2 hi=lo+1.0f; float2 q0=float2(lo.x,lo.y), q1=float2(hi.x,lo.y), q2=float2(hi.x,hi.y), q3=float2(lo.x,hi.y); if (m12_cons_point_in_box(a,lo,hi) || m12_cons_point_in_box(b,lo,hi) || m12_cons_point_in_box(c,lo,hi)) return true; if (m12_cons_point_in_tri(q0,a,b,c) || m12_cons_point_in_tri(q1,a,b,c) || m12_cons_point_in_tri(q2,a,b,c) || m12_cons_point_in_tri(q3,a,b,c)) return true; return m12_cons_seg_intersects(a,b,q0,q1) || m12_cons_seg_intersects(a,b,q1,q2) || m12_cons_seg_intersects(a,b,q2,q3) || m12_cons_seg_intersects(a,b,q3,q0) || m12_cons_seg_intersects(b,c,q0,q1) || m12_cons_seg_intersects(b,c,q1,q2) || m12_cons_seg_intersects(b,c,q2,q3) || m12_cons_seg_intersects(b,c,q3,q0) || m12_cons_seg_intersects(c,a,q0,q1) || m12_cons_seg_intersects(c,a,q1,q2) || m12_cons_seg_intersects(c,a,q2,q3) || m12_cons_seg_intersects(c,a,q3,q0); }\n";
        }
        if (ctx.options.depth_bounds_test)
            ctx.binding_plan.direct_buffer_count =
                std::min<uint32_t>(ctx.binding_plan.direct_buffer_count, 28);
        os << "fragment float4 ps_main(\n";
        os << "  input_v in [[stage_in]],\n";
        if (ctx.uses_sample_index)
            os << "  uint m12_sample_id [[sample_id]],\n";
        if (ctx.uses_coverage)
            os << "  uint m12_coverage [[sample_mask]],\n";
        for (uint32_t i = 0; i < ctx.binding_plan.direct_buffer_count; i++) {
            if (accelerationStructureAtBufferSlot(ctx, i))
                os << "  instance_acceleration_structure as" << i
                   << " [[buffer(" << i << ")]],\n";
            else
                os << "  device char* buf" << i << " [[buffer(" << i
                   << ")]],\n";
        }
        if (ctx.options.conservative_rasterization)
            os << "  constant m12_conservative_data& m12_conservative [[buffer(26)]],\n";
        for (uint32_t i = 0; i < ctx.binding_plan.direct_texture_count; i++) {
            bool comparison_slot =
                ctx.sample_cmp_shader &&
                ctx.comparison_texture_slots.count(i) != 0;
            bool srv_slot = false;
            bool uav_slot = false;
            for (const auto &range : ctx.binding_plan.ranges) {
                if (range.kind == DescriptorRangePlan::Kind::SRV &&
                    i >= range.lower_bound &&
                    i < range.lower_bound + range.count)
                    srv_slot = true;
                if (range.kind == DescriptorRangePlan::Kind::UAV &&
                    i >= range.lower_bound &&
                    i < range.lower_bound + range.count)
                    uav_slot = true;
            }
            uint32_t resource_kind = resourceKindForTextureSlot(ctx, i);
            uint32_t element_type = resourceElementTypeForTextureSlot(ctx, i);
            bool integer_element = isIntegerResourceElementType(element_type);
            bool signed_integer = isSignedResourceElementType(element_type);
            if (resource_kind != 0u) {
                bool texture_uav_slot = textureSlotHasRangeKind(
                    ctx, i, DescriptorRangePlan::Kind::UAV);
                bool writable_msaa = texture_uav_slot &&
                    (isTextureMSAAResourceKind(resource_kind) ||
                     ctx.texture_store_sample_shader ||
                     ctx.writable_msaa_texture_slots.count(i));
                if (comparison_slot && srv_slot && !uav_slot &&
                    (resource_kind == 1u || resource_kind == 2u ||
                     resource_kind == 5u || resource_kind == 6u ||
                     resource_kind == 7u || resource_kind == 9u))
                    os << "  " << depthTextureBindingType(resource_kind)
                       << " tex" << i << " [[texture(" << i << ")]],\n";
                else if (uav_slot && ctx.uses_sampler_feedback && srv_slot)
                    os << "  " << textureBindingType(resource_kind, false, false, true)
                       << " tex" << i << " [[texture(" << i << ")]],\n";
                else
                    os << "  " << textureBindingType(
                               resource_kind, texture_uav_slot, integer_element,
                               !texture_uav_slot, writable_msaa, signed_integer)
                       << " tex" << i << " [[texture(" << i << ")]],\n";
            } else if (comparison_slot && srv_slot && !uav_slot)
                os << "  depth2d<float, access::sample> tex" << i << " [[texture(" << i << ")]],\n";
            else if (uav_slot && ctx.uses_sampler_feedback && srv_slot)
                os << "  texture2d<float, access::sample> tex" << i << " [[texture(" << i << ")]],\n";
            else if (uav_slot && (ctx.texture_store_sample_shader ||
                                  ctx.writable_msaa_texture_slots.count(i)))
                os << "  texture2d_array<float, access::read_write> tex" << i << " [[texture(" << i << ")]],\n";
            else if (uav_slot)
                os << "  texture2d<float, access::read_write> tex" << i << " [[texture(" << i << ")]],\n";
            else
                os << "  texture2d<float, access::sample> tex" << i << " [[texture(" << i << ")]],\n";
        }
        if (ctx.options.vrs_per_primitive)
            os << "  texture2d<float, access::write> m12_vrs_mask [[texture(125)]],\n";
        for (uint32_t i = 0; i < ctx.binding_plan.direct_sampler_count; i++) {
            os << "  sampler samp" << i << " [[sampler(" << i << ")]]";
            os << (i + 1 == ctx.binding_plan.direct_sampler_count &&
                           !ctx.options.depth_bounds_test &&
                           !ctx.options.vrs_per_primitive
                       ? "\n"
                       : ",\n");
        }
        if (ctx.options.vrs_per_primitive) {
            os << "  constant uint4& m12_vrs_state [[buffer(27)]]";
            os << (ctx.options.depth_bounds_test ? ",\n" : "\n");
        }
        if (ctx.options.depth_bounds_test) {
            os << "  constant float4& m12_depth_bounds [[buffer(28)]],\n";
            os << (ctx.options.depth_bounds_multisample
                       ? "  depth2d_ms_array<float, access::read> "
                       : "  depth2d_array<float, access::read> ")
               <<
                  "m12_depth_bounds_texture [[texture(126)]],\n";
            os << "  uint m12_depth_layer [[render_target_array_index]]";
            if (ctx.options.depth_bounds_multisample)
                os << ",\n  uint m12_depth_sample [[sample_id]]\n";
            else
                os << "\n";
        }
        os << ") {\n";
        if (ctx.options.conservative_rasterization) {
            os << "  if (m12_conservative.enabled != 0u && !m12_cons_triangle_pixel(m12_conservative.p0, m12_conservative.p1, m12_conservative.p2, floor(in.position.xy))) discard_fragment();\n";
        }
        if (ctx.options.vrs_per_primitive) {
            if (ctx.shader.shading_rate_input_register >= 0)
                os << "  if (m12_vrs_state.x != 0xffffffffu && "
                       "in.shading_rate != m12_vrs_state.x) "
                       "discard_fragment();\n";
            os << "  if (m12_vrs_state.z != 0u) {\n";
            // With a Metal rate map, fragment positions are in the physical
            // framebuffer coordinate space.  The mask follows that same
            // space; the resolve shader maps logical screen coordinates back
            // to it through rasterization_rate_map_data.
            os << "    m12_vrs_mask.write(float4(1.0f), uint2(floor(in.position.xy)));\n";
            os << "  }\n";
        }
        if (ctx.options.depth_bounds_test) {
            os << "  float m12_stored_depth = m12_depth_bounds_texture.read("
                  "uint2(in.position.xy), m12_depth_layer + "
                  "uint(m12_depth_bounds.z)";
            if (ctx.options.depth_bounds_multisample)
                os << ", m12_depth_sample";
            os << ");\n";
            os << "  if (m12_depth_bounds.w != 0.0f || "
                  "m12_stored_depth < m12_depth_bounds.x || "
                  "m12_stored_depth > m12_depth_bounds.y) "
                  "discard_fragment();\n";
        }
        os << "  float4 result = float4(0,0,0,1);\n";
        emitTempRegisterDeclarations(ctx);
    } else {
        os << "kernel void unknown_main() {\n";
    }
}

static std::string resolveValue(LowerContext &ctx, uint32_t idx) {
    if (idx < ctx.value_table.size() && !ctx.value_table[idx].empty()) {
        const auto &v = ctx.value_table[idx];
        if (startsWith(v, "dx.")) {
            // function name, not a resolvable value
        } else if (startsWith(v, "agg(")) {
            MSLType type = idx < ctx.value_types.size() ? ctx.value_types[idx] : MSLType{};
            return aggregateConstructor(v, type);
        } else if (v.find('.') != std::string::npos) {
            return v;
        } else {
            return v;
        }
    }
    for (auto &c : ctx.mod.constants) {
        if (c.id == idx && !c.constant_data.empty())
            return normalizeAggregateExpressions(c.constant_data, DXILIRBuilder::resolveType(c.type_id, ctx.mod));
    }
    if (ctx.current_fn) {
        for (auto &c : ctx.current_fn->constants) {
            if (c.id == idx && !c.constant_data.empty())
                return normalizeAggregateExpressions(c.constant_data, DXILIRBuilder::resolveType(c.type_id, ctx.mod));
        }
    }
    return emitValue(idx);
}

static bool exprLooksResourceHandle(const std::string &value) {
    const std::string &stripped = value;
    auto is_numbered_handle = [&](const char *prefix) {
        const size_t prefix_length = std::strlen(prefix);
        if (!startsWith(stripped, prefix) || stripped.size() == prefix_length)
            return false;
        for (size_t i = prefix_length; i < stripped.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(stripped[i])))
                return false;
        return true;
    };
    return is_numbered_handle("tex") || is_numbered_handle("samp") ||
           is_numbered_handle("buf") || is_numbered_handle("as");
}

static bool exprContainsPointerSyntax(const std::string &value) {
    return value.find("char*") != std::string::npos ||
           value.find("char *") != std::string::npos ||
           value.find("thread char") != std::string::npos ||
           value.find("threadgroup char") != std::string::npos ||
           value.find("device char") != std::string::npos ||
           value.find("&alloca_") != std::string::npos;
}

static bool typeLooksResourceHandle(const MSLType &type) {
    switch (type.kind) {
    case MSLTypeKind::DeviceCharPtr:
    case MSLTypeKind::ThreadgroupCharPtr:
    case MSLTypeKind::Texture2D:
    case MSLTypeKind::Texture2DArray:
    case MSLTypeKind::Texture3D:
    case MSLTypeKind::TextureCube:
    case MSLTypeKind::Texture2DMS:
    case MSLTypeKind::RWTexture2D:
    case MSLTypeKind::RWTexture2DArray:
    case MSLTypeKind::RWTexture3D:
    case MSLTypeKind::Sampler:
    case MSLTypeKind::InstanceAccelerationStructure:
        return true;
    default:
        return false;
    }
}

static std::string stripEnclosingParens(std::string value);
static bool exprEndsWithComponent(const std::string &value);

static bool isUsableMSLType(const MSLType &type) {
    return type.kind != MSLTypeKind::Unknown && type.kind != MSLTypeKind::Void &&
           type.kind != MSLTypeKind::Struct;
}

static MSLType mergePredeclType(const MSLType &current, const MSLType &incoming) {
    if (!isUsableMSLType(current))
        return incoming;
    if (!isUsableMSLType(incoming))
        return current;
    if (current.kind == incoming.kind)
        return current;

    bool current_resource = typeLooksResourceHandle(current);
    bool incoming_resource = typeLooksResourceHandle(incoming);
    if (current_resource && !incoming_resource)
        return incoming;
    if (!current_resource && incoming_resource)
        return current;
    if (current_resource && incoming_resource) {
        if (current.kind == MSLTypeKind::RWTexture2D || incoming.kind == MSLTypeKind::RWTexture2D)
            return {MSLTypeKind::RWTexture2D, 0, {}};
        if (current.kind == MSLTypeKind::RWTexture2DArray || incoming.kind == MSLTypeKind::RWTexture2DArray)
            return {MSLTypeKind::RWTexture2DArray, 0, {}};
        if (current.kind == MSLTypeKind::RWTexture3D || incoming.kind == MSLTypeKind::RWTexture3D)
            return {MSLTypeKind::RWTexture3D, 0, {}};
        return current;
    }

    if (DXILIRBuilder::isVectorType(current))
        return current;
    if (DXILIRBuilder::isVectorType(incoming))
        return incoming;
    if (DXILIRBuilder::isFloatType(current))
        return current;
    if (DXILIRBuilder::isFloatType(incoming))
        return incoming;
    if (current.kind == MSLTypeKind::Bool)
        return current;
    if (incoming.kind == MSLTypeKind::Bool)
        return incoming;
    if (current.kind == MSLTypeKind::UInt || incoming.kind == MSLTypeKind::UInt)
        return {MSLTypeKind::UInt, 0, {}};
    return current;
}

static bool splitTrailingComponentAccess(const std::string &value, std::string &base) {
    std::string stripped = stripEnclosingParens(value);
    if (stripped.size() < 3)
        return false;
    char c = stripped.back();
    if (c != 'x' && c != 'y' && c != 'z' && c != 'w' &&
        c != 'r' && c != 'g' && c != 'b' && c != 'a')
        return false;
    if (stripped[stripped.size() - 2] != '.')
        return false;
    base = stripEnclosingParens(stripped.substr(0, stripped.size() - 2));
    return !base.empty();
}

static MSLType typeForResolvedValueName(const LowerContext &ctx, const std::string &value) {
    auto pre_it = ctx.predeclared_types.find(value);
    if (pre_it != ctx.predeclared_types.end())
        return pre_it->second;

    uint32_t source_id = 0;
    if (parseEmittedValueName(value, source_id) && source_id < ctx.value_types.size())
        return ctx.value_types[source_id];

    return {};
}

static bool exprContainsPointerTypedValue(const LowerContext &ctx, const std::string &value) {
    size_t pos = value.find('v');
    while (pos != std::string::npos) {
        bool start_ok = pos == 0 ||
            (!std::isalnum((unsigned char)value[pos - 1]) && value[pos - 1] != '_');
        size_t end = pos + 1;
        while (end < value.size() && std::isdigit((unsigned char)value[end]))
            end++;
        bool has_digits = end > pos + 1;
        bool end_ok = end >= value.size() ||
            (!std::isalnum((unsigned char)value[end]) && value[end] != '_');
        if (start_ok && has_digits && end_ok) {
            MSLType type = typeForResolvedValueName(ctx, value.substr(pos, end - pos));
            if (typeLooksResourceHandle(type))
                return true;
        }
        pos = value.find('v', pos + 1);
    }
    return false;
}

static MSLType firstVectorTypedValueType(const LowerContext &ctx, const std::string &value) {
    size_t pos = value.find('v');
    while (pos != std::string::npos) {
        bool start_ok = pos == 0 ||
            (!std::isalnum((unsigned char)value[pos - 1]) && value[pos - 1] != '_');
        size_t end = pos + 1;
        while (end < value.size() && std::isdigit((unsigned char)value[end]))
            end++;
        bool has_digits = end > pos + 1;
        bool end_ok = end >= value.size() ||
            (!std::isalnum((unsigned char)value[end]) && value[end] != '_');
        if (start_ok && has_digits && end_ok) {
            MSLType type = typeForResolvedValueName(ctx, value.substr(pos, end - pos));
            if (DXILIRBuilder::isVectorType(type))
                return type;
        }
        pos = value.find('v', pos + 1);
    }
    return {};
}

static bool exprContainsVectorTypedValue(const LowerContext &ctx, const std::string &value) {
    return DXILIRBuilder::isVectorType(firstVectorTypedValueType(ctx, value));
}

static bool exprContainsBareVectorTypedValue(const LowerContext &ctx, const std::string &value) {
    size_t pos = value.find('v');
    while (pos != std::string::npos) {
        bool start_ok = pos == 0 ||
            (!std::isalnum((unsigned char)value[pos - 1]) && value[pos - 1] != '_');
        size_t end = pos + 1;
        while (end < value.size() && std::isdigit((unsigned char)value[end]))
            end++;
        bool has_digits = end > pos + 1;
        bool end_ok = end >= value.size() ||
            (!std::isalnum((unsigned char)value[end]) && value[end] != '_');
        if (start_ok && has_digits && end_ok) {
            MSLType type = typeForResolvedValueName(ctx, value.substr(pos, end - pos));
            if (DXILIRBuilder::isVectorType(type)) {
                size_t lookahead = end;
                while (lookahead < value.size() && std::isspace((unsigned char)value[lookahead]))
                    lookahead++;
                size_t wrapped = lookahead;
                while (wrapped < value.size() && value[wrapped] == ')')
                    wrapped++;
                while (wrapped < value.size() && std::isspace((unsigned char)value[wrapped]))
                    wrapped++;
                if ((lookahead >= value.size() || value[lookahead] != '.') &&
                    (wrapped >= value.size() || value[wrapped] != '.'))
                    return true;
            }
        }
        pos = value.find('v', pos + 1);
    }
    return false;
}

static MSLType typeForResolvedExpression(const LowerContext &ctx, const std::string &value) {
    MSLType direct = typeForResolvedValueName(ctx, value);
    if (isUsableMSLType(direct))
        return direct;

    std::string base;
    if (splitTrailingComponentAccess(value, base)) {
        MSLType base_type = typeForResolvedExpression(ctx, base);
        if (DXILIRBuilder::isVectorType(base_type))
            return DXILIRBuilder::scalarType(base_type);
        if (isUsableMSLType(base_type))
            return base_type;
    }

    if (value.find(".calculate_clamped_lod(") != std::string::npos ||
        value.find(".calculate_unclamped_lod(") != std::string::npos)
        return {MSLTypeKind::Float, 0, {}};
    if (value.find("m12_dynamic_buffer_load_") != std::string::npos)
        return {MSLTypeKind::UInt4, 0, {}};
    if (value.find("reinterpret_cast<device float4&>") != std::string::npos ||
        value.find(".read(") != std::string::npos ||
        value.find(".sample(") != std::string::npos ||
        value.find(".gather(") != std::string::npos ||
        value.find(".gather_compare(") != std::string::npos)
        return {MSLTypeKind::Float4, 0, {}};
    if (value.find("reinterpret_cast<device uint4&>") != std::string::npos)
        return {MSLTypeKind::UInt4, 0, {}};
    if (value.find("reinterpret_cast<device int4&>") != std::string::npos)
        return {MSLTypeKind::Int4, 0, {}};
    if (value.find("reinterpret_cast<device float&>") != std::string::npos)
        return {MSLTypeKind::Float, 0, {}};
    if (value.find("reinterpret_cast<device uint&>") != std::string::npos)
        return {MSLTypeKind::UInt, 0, {}};
    if (value.find("reinterpret_cast<device int&>") != std::string::npos)
        return {MSLTypeKind::Int, 0, {}};

    if (exprContainsPointerSyntax(value))
        return {MSLTypeKind::DeviceCharPtr, 0, {}};

    return {};
}

static bool exprLooksScalarLiteral(const std::string &value) {
    if (value == "true" || value == "false" ||
        value == "INFINITY" || value == "-INFINITY" || value == "NAN")
        return true;
    if (value.empty())
        return false;
    char *end = nullptr;
    std::strtod(value.c_str(), &end);
    if (end && (*end == '\0' || ((*end == 'f' || *end == 'u') && end[1] == '\0')))
        return true;
    return false;
}

static bool exprLooksSideEffectOnly(const std::string &value) {
    return value.find(".write(") != std::string::npos ||
           value.find("threadgroup_barrier(") != std::string::npos ||
           startsWith(stripEnclosingParens(value), "out.") ||
           startsWith(stripEnclosingParens(value), "result.");
}

static bool exprContainsAssignment(const std::string &value) {
    for (size_t pos = value.find('='); pos != std::string::npos;
         pos = value.find('=', pos + 1)) {
        const char previous = pos == 0 ? '\0' : value[pos - 1];
        const char next = pos + 1 < value.size() ? value[pos + 1] : '\0';
        if (previous != '=' && previous != '!' && previous != '<' &&
            previous != '>' && next != '=')
            return true;
    }
    return false;
}

static bool exprLooksScalarMathCall(const std::string &value) {
    std::string stripped = stripEnclosingParens(value);
    static const char *math_calls[] = {
        "abs(", "acos(", "asin(", "atan(", "ceil(", "cos(", "exp(", "fabs(",
        "floor(", "log(", "log2(", "rint(", "round(", "rsqrt(", "sin(",
        "sqrt(", "tan(", "trunc("
    };
    for (const char *call : math_calls)
        if (startsWith(stripped, call))
            return true;
    return false;
}

static bool exprLooksScalarCast(const std::string &value) {
    std::string stripped = stripEnclosingParens(value);
    return startsWith(stripped, "static_cast<int>(") ||
           startsWith(stripped, "static_cast<uint>(") ||
           startsWith(stripped, "static_cast<float>(") ||
           startsWith(stripped, "static_cast<bool>(");
}

static bool exprLooksThreadVector(const std::string &value) {
    std::string stripped = stripEnclosingParens(value);
    return stripped == "dtid" || stripped == "gtid" ||
           stripped == "ggid" || stripped == "gsz";
}

static void replaceBareThreadVectorCast(std::string &value, const std::string &from,
                                        const std::string &to) {
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        size_t end = pos + from.size();
        bool bare = end >= value.size() ||
                    (!std::isalnum((unsigned char)value[end]) &&
                     value[end] != '_' && value[end] != '.');
        if (bare) {
            value.replace(pos, from.size(), to);
            pos += to.size();
        } else {
            pos = end;
        }
    }
}

static std::string sanitizeThreadVectorCasts(std::string value) {
    for (const char *name : {"dtid", "gtid", "ggid", "gsz"}) {
        std::string base(name);
        replaceBareThreadVectorCast(value, "(int)" + base, "(int)" + base + ".x");
        replaceBareThreadVectorCast(value, "(uint)" + base, "(uint)" + base + ".x");
        replaceBareThreadVectorCast(value, "static_cast<int>(" + base + ")",
                                    "static_cast<int>(" + base + ".x)");
        replaceBareThreadVectorCast(value, "static_cast<uint>(" + base + ")",
                                    "static_cast<uint>(" + base + ".x)");
    }
    return value;
}

static bool exprLooksBoolValue(const std::string &value) {
    std::string stripped = stripEnclosingParens(value);
    return stripped == "true" || stripped == "false" ||
           stripped.find(" == ") != std::string::npos ||
           stripped.find(" != ") != std::string::npos ||
           stripped.find(" <= ") != std::string::npos ||
           stripped.find(" >= ") != std::string::npos ||
           stripped.find(" < ") != std::string::npos ||
           stripped.find(" > ") != std::string::npos ||
           startsWith(stripped, "any(") ||
           startsWith(stripped, "all(") ||
           startsWith(stripped, "isnan(") ||
           startsWith(stripped, "!isnan(");
}

static std::string scalarizeVectorBoolExpression(const std::string &expr,
                                                 const MSLType &source_type,
                                                 bool require_all = false) {
    if (!DXILIRBuilder::isVectorType(source_type))
        return expr;
    return std::string(require_all ? "all((" : "any((") + expr + "))";
}

static MSLType floatVectorTypeForWidth(uint32_t width) {
    switch (width) {
    case 2: return {MSLTypeKind::Float2, 0, {}};
    case 3: return {MSLTypeKind::Float3, 0, {}};
    case 4: return {MSLTypeKind::Float4, 0, {}};
    default: return {MSLTypeKind::Float, 0, {}};
    }
}

static std::string coerceIsNanOperand(const std::string &value, const MSLType &source_type) {
    if (!DXILIRBuilder::isVectorType(source_type))
        return "static_cast<float>(" + value + ")";
    MSLType scalar = DXILIRBuilder::scalarType(source_type);
    if (DXILIRBuilder::isFloatType(scalar))
        return value;
    MSLType float_vector = floatVectorTypeForWidth(DXILIRBuilder::vectorWidth(source_type));
    std::string type_name = emitTypeName(float_vector);
    return type_name.empty() ? value : type_name + "(" + value + ")";
}

static std::string vectorZeroForExpression(const std::string &value) {
    std::string stripped = stripEnclosingParens(value);
    static const std::pair<const char *, const char *> zeros[] = {
        {"float2(", "float2(0.0f)"}, {"float3(", "float3(0.0f)"},
        {"float4(", "float4(0.0f)"}, {"uint2(", "uint2(0)"},
        {"uint3(", "uint3(0)"},     {"uint4(", "uint4(0)"},
        {"int2(", "int2(0)"},       {"int3(", "int3(0)"},
        {"int4(", "int4(0)"},
    };
    for (const auto &zero : zeros) {
        if (startsWith(stripped, zero.first) ||
            stripped.find(zero.first) != std::string::npos)
            return zero.second;
    }
    if (stripped.find("reinterpret_cast<device float4&>") != std::string::npos ||
        stripped.find(".read(") != std::string::npos ||
        stripped.find(".sample(") != std::string::npos ||
        stripped.find(".gather(") != std::string::npos ||
        stripped.find(".gather_compare(") != std::string::npos)
        return "float4(0.0f)";
    if (stripped.find("reinterpret_cast<device uint4&>") != std::string::npos ||
        stripped.find("m12_dynamic_buffer_load_") != std::string::npos)
        return "uint4(0)";
    if (stripped.find("reinterpret_cast<device int4&>") != std::string::npos)
        return "int4(0)";
    return "";
}

static bool exprLooksScalarResultCall(const std::string &value) {
    std::string stripped = stripEnclosingParens(value);
    static const char *scalar_calls[] = {
        "any(", "all(", "dot(", "length(", "distance("
    };
    if (stripped.find(".calculate_clamped_lod(") != std::string::npos ||
        stripped.find(".calculate_unclamped_lod(") != std::string::npos)
        return true;
    for (const char *call : scalar_calls)
        if (startsWith(stripped, call))
            return true;
    return false;
}

static bool exprContainsBareStageInputVector(const std::string &value) {
    size_t pos = value.find("in.v");
    while (pos != std::string::npos) {
        bool start_ok = pos == 0 || (!std::isalnum((unsigned char)value[pos - 1]) && value[pos - 1] != '_');
        size_t idx = pos + 4;
        if (start_ok && idx < value.size() && std::isdigit((unsigned char)value[idx])) {
            while (idx < value.size() && std::isdigit((unsigned char)value[idx]))
                idx++;
            size_t use = idx;
            while (use < value.size() && std::isspace((unsigned char)value[use]))
                use++;
            bool scalar_access = use < value.size() && (value[use] == '.' || value[use] == '[');
            bool end_ok = use >= value.size() || (!std::isalnum((unsigned char)value[use]) && value[use] != '_');
            if (!scalar_access && end_ok)
                return true;
        }
        pos = value.find("in.v", pos + 1);
    }
    return false;
}

static bool exprLooksVectorValue(const std::string &value) {
    if (exprEndsWithComponent(value) || exprLooksScalarResultCall(value))
        return false;
    if (exprContainsBareStageInputVector(value))
        return true;
    return startsWith(value, "float2(") || startsWith(value, "float3(") ||
           startsWith(value, "float4(") || startsWith(value, "int2(") ||
           startsWith(value, "int3(") || startsWith(value, "int4(") ||
           startsWith(value, "uint2(") || startsWith(value, "uint3(") ||
           startsWith(value, "uint4(") ||
           value.find("float2(") != std::string::npos ||
           value.find("float3(") != std::string::npos ||
           value.find("float4(") != std::string::npos ||
           value.find("int2(") != std::string::npos ||
           value.find("int3(") != std::string::npos ||
           value.find("int4(") != std::string::npos ||
           value.find("uint2(") != std::string::npos ||
           value.find("uint3(") != std::string::npos ||
           value.find("uint4(") != std::string::npos ||
           value.find("reinterpret_cast<device float4&>") != std::string::npos ||
           value.find("reinterpret_cast<device uint4&>") != std::string::npos ||
           value.find("reinterpret_cast<device int4&>") != std::string::npos ||
           value.find(".read(") != std::string::npos ||
           value.find(".sample(") != std::string::npos ||
           value.find(".gather(") != std::string::npos ||
           value.find(".gather_compare(") != std::string::npos ||
           value.find("m12_dynamic_buffer_load_") != std::string::npos;
}

static bool exprContainsVectorConstructor(const std::string &value) {
    static const char *constructors[] = {
        "float2(", "float3(", "float4(",
        "int2(", "int3(", "int4(",
        "uint2(", "uint3(", "uint4("
    };
    for (const char *ctor : constructors)
        if (value.find(ctor) != std::string::npos)
            return true;
    return false;
}

static bool parseConstructorArguments(const std::string &value,
                                      const std::string &constructor,
                                      std::vector<std::string> &args) {
    std::string trimmed = value;
    while (!trimmed.empty() && std::isspace((unsigned char)trimmed.front()))
        trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back()))
        trimmed.pop_back();

    std::string prefix = constructor + "(";
    if (!startsWith(trimmed, prefix.c_str()) || trimmed.back() != ')')
        return false;

    int depth = 0;
    size_t arg_start = prefix.size();
    for (size_t i = prefix.size(); i + 1 < trimmed.size(); i++) {
        char c = trimmed[i];
        if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) depth--;
        } else if (c == ',' && depth == 0) {
            args.push_back(trimmed.substr(arg_start, i - arg_start));
            arg_start = i + 1;
        }
    }
    args.push_back(trimmed.substr(arg_start, trimmed.size() - arg_start - 1));
    return true;
}

static std::string normalizeVectorConstructorArities(const std::string &value) {
    struct ConstructorInfo {
        const char *prefix;
        const char *name;
        size_t width;
    };
    static const ConstructorInfo constructors[] = {
        {"float2(", "float2", 2}, {"float3(", "float3", 3}, {"float4(", "float4", 4},
        {"int2(", "int2", 2},     {"int3(", "int3", 3},     {"int4(", "int4", 4},
        {"uint2(", "uint2", 2},   {"uint3(", "uint3", 3},   {"uint4(", "uint4", 4},
        {"half2(", "half2", 2},   {"half3(", "half3", 3},   {"half4(", "half4", 4},
    };

    auto find_matching_close = [](const std::string &text, size_t open_pos) -> size_t {
        int depth = 0;
        for (size_t i = open_pos; i < text.size(); i++) {
            char c = text[i];
            if (c == '(' || c == '[' || c == '{') {
                depth++;
            } else if (c == ')' || c == ']' || c == '}') {
                depth--;
                if (depth == 0)
                    return i;
            }
        }
        return std::string::npos;
    };

    std::string out;
    size_t pos = 0;
    bool changed = false;
    while (pos < value.size()) {
        size_t best = std::string::npos;
        const ConstructorInfo *match = nullptr;
        for (const auto &ctor : constructors) {
            size_t found = value.find(ctor.prefix, pos);
            while (found != std::string::npos) {
                bool boundary = found == 0 || (!std::isalnum((unsigned char)value[found - 1]) && value[found - 1] != '_');
                if (boundary)
                    break;
                found = value.find(ctor.prefix, found + 1);
            }
            if (found != std::string::npos && (best == std::string::npos || found < best)) {
                best = found;
                match = &ctor;
            }
        }

        if (!match) {
            out += value.substr(pos);
            break;
        }

        out += value.substr(pos, best - pos);
        size_t open_pos = best + std::strlen(match->name);
        size_t close_pos = find_matching_close(value, open_pos);
        if (close_pos == std::string::npos) {
            out += value.substr(best);
            break;
        }

        std::string segment = value.substr(best, close_pos - best + 1);
        std::vector<std::string> args;
        if (!parseConstructorArguments(segment, match->name, args)) {
            out += segment;
            pos = close_pos + 1;
            continue;
        }

        auto trim = [](std::string text) {
            while (!text.empty() && std::isspace((unsigned char)text.front()))
                text.erase(text.begin());
            while (!text.empty() && std::isspace((unsigned char)text.back()))
                text.pop_back();
            return text;
        };
        auto swizzle_width_for = [&](const std::string &expr) -> size_t {
            std::string stripped = stripEnclosingParens(trim(expr));
            size_t dot = stripped.rfind('.');
            if (dot == std::string::npos || dot + 2 > stripped.size())
                return 1;
            size_t width = stripped.size() - dot - 1;
            if (width < 2 || width > 4)
                return 1;
            for (size_t i = dot + 1; i < stripped.size(); i++) {
                char c = stripped[i];
                if (c != 'x' && c != 'y' && c != 'z' && c != 'w' && c != 'r' && c != 'g' && c != 'b' && c != 'a')
                    return 1;
            }
            return width;
        };
        auto constructor_width_for = [&](const std::string &expr) -> size_t {
            std::string stripped = stripEnclosingParens(trim(expr));
            size_t swizzle_width = swizzle_width_for(stripped);
            if (swizzle_width > 1)
                return swizzle_width;
            for (const auto &ctor : constructors) {
                if (startsWith(stripped, ctor.prefix) && stripped.size() > std::strlen(ctor.prefix) && stripped.back() == ')')
                    return ctor.width;
            }
            return 1;
        };
        auto zero_for_constructor = [&]() -> std::string {
            if (startsWith(match->name, "float") || startsWith(match->name, "half"))
                return "0.0f";
            if (startsWith(match->name, "uint"))
                return "0u";
            return "0";
        };

        bool segment_changed = args.size() > match->width;
        std::vector<std::string> normalized_args;
        normalized_args.reserve(match->width);
        for (size_t i = 0; i < args.size() && normalized_args.size() < match->width; i++) {
            std::string arg = trim(args[i]);
            std::string normalized_arg = normalizeVectorConstructorArities(arg);
            if (normalized_arg != arg)
                segment_changed = true;
            size_t arg_width = constructor_width_for(normalized_arg);
            if (arg_width > 1) {
                segment_changed = true;
                for (size_t component = 0; component < arg_width && normalized_args.size() < match->width; component++)
                    normalized_args.push_back("(" + normalized_arg + ")" + componentSuffix((uint32_t)component));
            } else {
                normalized_args.push_back(normalized_arg);
            }
        }
        if (segment_changed) {
            while (normalized_args.size() < match->width)
                normalized_args.push_back(zero_for_constructor());
        }

        if (segment_changed) {
            changed = true;
            out += match->name;
            out += "(";
            for (size_t i = 0; i < normalized_args.size(); i++) {
                if (i)
                    out += ", ";
                out += normalized_args[i];
            }
            out += ")";
        } else {
            out += segment;
        }
        pos = close_pos + 1;
    }

    return changed ? out : value;
}

static bool startsWithVectorConstructor(std::string value) {
    while (!value.empty() && std::isspace((unsigned char)value.front()))
        value.erase(value.begin());
    static const char *constructors[] = {
        "float2(", "float3(", "float4(",
        "int2(", "int3(", "int4(",
        "uint2(", "uint3(", "uint4("
    };
    for (const char *ctor : constructors)
        if (startsWith(value, ctor))
            return true;
    return false;
}

static std::string scalarizeNestedVectorConstructorArgs(const std::string &value,
                                                        const MSLType &target) {
    if (!DXILIRBuilder::isVectorType(target))
        return value;

    std::string type_name = emitTypeName(target);
    if (type_name.empty() || type_name == "auto")
        return value;

    std::vector<std::string> args;
    if (!parseConstructorArguments(value, type_name, args) || args.size() <= 1)
        return value;

    bool changed = false;
    std::ostringstream out;
    out << type_name << "(";
    for (size_t i = 0; i < args.size(); i++) {
        std::string arg = args[i];
        while (!arg.empty() && std::isspace((unsigned char)arg.front()))
            arg.erase(arg.begin());
        while (!arg.empty() && std::isspace((unsigned char)arg.back()))
            arg.pop_back();
        if (startsWithVectorConstructor(arg)) {
            arg = "(" + arg + ").x";
            changed = true;
        }
        if (i) out << ", ";
        out << arg;
    }
    out << ")";
    return changed ? out.str() : value;
}

static bool splitTopLevelTernary(const std::string &value,
                                 std::string &then_branch,
                                 std::string &else_branch) {
    std::string stripped = stripEnclosingParens(value);
    int depth = 0;
    int ternary_depth = 0;
    size_t question = std::string::npos;
    for (size_t i = 0; i < stripped.size(); i++) {
        char c = stripped[i];
        if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) depth--;
        } else if (depth == 0 && c == '?') {
            if (question == std::string::npos)
                question = i;
            ternary_depth++;
        } else if (depth == 0 && c == ':' && ternary_depth > 0) {
            ternary_depth--;
            if (ternary_depth == 0 && question != std::string::npos) {
                then_branch = stripEnclosingParens(stripped.substr(question + 1, i - question - 1));
                else_branch = stripEnclosingParens(stripped.substr(i + 1));
                return !then_branch.empty() && !else_branch.empty();
            }
        }
    }
    return false;
}

static bool exprHasTopLevelComparison(const std::string &value) {
    std::string stripped = stripEnclosingParens(value);
    int depth = 0;
    for (size_t i = 0; i < stripped.size(); i++) {
        char c = stripped[i];
        if (c == '(' || c == '[' || c == '{') {
            depth++;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) depth--;
            continue;
        }
        if (depth != 0)
            continue;

        if (i + 1 < stripped.size()) {
            std::string op = stripped.substr(i, 2);
            if (op == "==" || op == "!=" || op == "<=" || op == ">=")
                return true;
        }
        if ((c == '<' || c == '>') &&
            (i == 0 || stripped[i - 1] != '<') &&
            (i + 1 >= stripped.size() || stripped[i + 1] != '>') &&
            i > 0 && i + 1 < stripped.size() &&
            std::isspace((unsigned char)stripped[i - 1]) &&
            std::isspace((unsigned char)stripped[i + 1]))
            return true;
    }
    return false;
}

static std::string scalarizeVectorOperands(const LowerContext &ctx, const std::string &expr);

static std::string coerceVectorBoolExpressionForAssignment(const LowerContext &ctx,
                                                           const std::string &value) {
    std::string resolved = sanitizeThreadVectorCasts(normalizeAggregateExpressions(value));
    std::string then_branch;
    std::string else_branch;
    if (splitTopLevelTernary(resolved, then_branch, else_branch)) {
        MSLType branch_type = typeForResolvedExpression(ctx, then_branch);
        if (!DXILIRBuilder::isVectorType(branch_type))
            branch_type = typeForResolvedExpression(ctx, else_branch);
        if (DXILIRBuilder::isVectorType(branch_type)) {
            std::string zero = vectorZeroForExpression(resolved);
            if (zero.empty())
                zero = defaultForType(branch_type);
            return "any((" + resolved + ") != " + zero + ")";
        }
        return "";
    }

    MSLType vector_type = typeForResolvedExpression(ctx, resolved);
    if (!DXILIRBuilder::isVectorType(vector_type))
        vector_type = firstVectorTypedValueType(ctx, resolved);
    if (!DXILIRBuilder::isVectorType(vector_type) && exprLooksVectorValue(resolved)) {
        if (exprHasTopLevelComparison(resolved))
            return "any((" + resolved + "))";
        std::string zero = vectorZeroForExpression(resolved);
        if (!zero.empty())
            return "any((" + resolved + ") != " + zero + ")";
    }
    if (!DXILIRBuilder::isVectorType(vector_type))
        return "";

    if (!exprLooksVectorValue(resolved) && !exprContainsBareVectorTypedValue(ctx, resolved))
        return "(" + resolved + " != 0)";

    if (exprHasTopLevelComparison(resolved) || startsWith(stripEnclosingParens(resolved), "isnan(") ||
        startsWith(stripEnclosingParens(resolved), "!isnan("))
        return "any((" + resolved + "))";

    std::string scalarized = scalarizeVectorOperands(ctx, resolved);
    if (scalarized != resolved && !exprLooksVectorValue(scalarized))
        return "(" + scalarized + " != 0)";

    std::string zero = vectorZeroForExpression(resolved);
    if (zero.empty())
        zero = defaultForType(vector_type);
    return "any((" + resolved + ") != " + zero + ")";
}

static bool exprContainsRawResourceHandle(const std::string &value) {
    if (value.find(".read(") != std::string::npos ||
        value.find(".sample(") != std::string::npos ||
        value.find(".gather(") != std::string::npos ||
        value.find(".gather_compare(") != std::string::npos ||
        value.find(".calculate_clamped_lod(") != std::string::npos ||
        value.find(".calculate_unclamped_lod(") != std::string::npos ||
        value.find(".write(") != std::string::npos ||
        value.find(".get_width(") != std::string::npos ||
        value.find(".get_height(") != std::string::npos)
        return false;

    for (const char *prefix : {"tex", "samp", "buf"}) {
        size_t pos = value.find(prefix);
        while (pos != std::string::npos) {
            bool start_ok = pos == 0 ||
                (!std::isalnum((unsigned char)value[pos - 1]) && value[pos - 1] != '_' && value[pos - 1] != '.');
            size_t end = pos + std::strlen(prefix);
            bool has_digits = false;
            while (end < value.size() && std::isdigit((unsigned char)value[end])) {
                has_digits = true;
                end++;
            }
            bool end_ok = end >= value.size() ||
                (!std::isalnum((unsigned char)value[end]) && value[end] != '_' && value[end] != '.');
            if (start_ok && has_digits && end_ok)
                return true;
            pos = value.find(prefix, pos + 1);
        }
    }
    return false;
}

static std::string coerceResolvedValue(const std::string &value, const MSLType &target) {
    std::string resolved = sanitizeThreadVectorCasts(normalizeAggregateExpressions(value, target));
    if (DXILIRBuilder::isVectorType(target))
        resolved = scalarizeNestedVectorConstructorArgs(resolved, target);
    if (target.kind == MSLTypeKind::Bool) {
        if (exprLooksResourceHandle(resolved) || exprLooksSideEffectOnly(resolved)) return "false";
        if (exprLooksBoolValue(resolved)) return resolved;
        if (exprLooksVectorValue(resolved)) {
            std::string zero = vectorZeroForExpression(resolved);
            if (!zero.empty())
                return "any((" + resolved + ") != " + zero + ")";
            return "((" + resolved + ").x != 0)";
        }
        return "(" + resolved + " != 0)";
    }
    if (exprLooksResourceHandle(resolved) &&
        target.kind != MSLTypeKind::DeviceCharPtr &&
        target.kind != MSLTypeKind::ThreadgroupCharPtr &&
        target.kind != MSLTypeKind::Texture2D &&
        target.kind != MSLTypeKind::Texture2DArray &&
        target.kind != MSLTypeKind::Texture3D &&
        target.kind != MSLTypeKind::TextureCube &&
        target.kind != MSLTypeKind::Texture2DMS &&
        target.kind != MSLTypeKind::RWTexture2D &&
        target.kind != MSLTypeKind::RWTexture2DArray &&
        target.kind != MSLTypeKind::RWTexture3D &&
        target.kind != MSLTypeKind::Sampler) {
        return defaultForType(target);
    }
    if ((target.kind == MSLTypeKind::DeviceCharPtr ||
         target.kind == MSLTypeKind::ThreadgroupCharPtr) &&
        (startsWith(resolved, "tex") || startsWith(resolved, "samp")))
        return defaultForType(target);
    if ((target.kind == MSLTypeKind::Texture2D ||
         target.kind == MSLTypeKind::Texture2DArray ||
         target.kind == MSLTypeKind::Texture3D ||
         target.kind == MSLTypeKind::TextureCube ||
         target.kind == MSLTypeKind::Texture2DMS ||
         target.kind == MSLTypeKind::RWTexture2D ||
         target.kind == MSLTypeKind::RWTexture2DArray ||
         target.kind == MSLTypeKind::RWTexture3D) &&
        !startsWith(resolved, "tex"))
        return defaultForType(target);
    if (target.kind == MSLTypeKind::Sampler && !startsWith(resolved, "samp"))
        return defaultForType(target);
    if ((target.kind == MSLTypeKind::DeviceCharPtr ||
         target.kind == MSLTypeKind::ThreadgroupCharPtr) &&
        (resolved.find("*(") != std::string::npos ||
         resolved.find("reinterpret_cast<device float") != std::string::npos ||
         resolved.find("reinterpret_cast<device uint") != std::string::npos ||
         resolved.find("reinterpret_cast<device int") != std::string::npos))
        return defaultForType(target);
    if ((target.kind == MSLTypeKind::DeviceCharPtr ||
         target.kind == MSLTypeKind::ThreadgroupCharPtr) &&
        !exprLooksResourceHandle(resolved) &&
        resolved.find("char*") == std::string::npos &&
        resolved.find("char *") == std::string::npos &&
        resolved.find("thread") == std::string::npos &&
        resolved.find("device") == std::string::npos) {
        return defaultForType(target);
    }
    if (exprLooksSideEffectOnly(resolved))
        return defaultForType(target);
    if ((DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) &&
        (exprContainsRawResourceHandle(resolved) || exprContainsPointerSyntax(resolved)) &&
        resolved.find("m12_load_vertex_attr(") == std::string::npos &&
        resolved.find("m12_dynamic_buffer_load_") == std::string::npos &&
        resolved.find("reinterpret_cast<device ") == std::string::npos &&
        resolved.find("reinterpret_cast<thread ") == std::string::npos &&
        resolved.find("reinterpret_cast<threadgroup ") == std::string::npos)
        return defaultForType(target);
    if ((DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) &&
        !DXILIRBuilder::isVectorType(target) && exprLooksVectorValue(resolved) &&
        !exprLooksScalarMathCall(resolved) && !exprLooksScalarCast(resolved))
        return "(" + normalizeVectorConstructorArities(resolved) + ").x";
    if (DXILIRBuilder::isVectorType(target) && !exprLooksVectorValue(resolved)) {
        std::string type_name = emitTypeName(target);
        if (!type_name.empty() && type_name != "auto")
            return type_name + "(" + resolved + ")";
    }
    if (DXILIRBuilder::isVectorType(target) && exprLooksVectorValue(resolved)) {
        std::string type_name = emitTypeName(target);
        std::string stripped = stripEnclosingParens(resolved);
        if (!type_name.empty() && type_name != "auto" &&
            !startsWith(stripped, (type_name + "(").c_str()))
            return type_name + "(" + resolved + ")";
    }
    if (resolved == "inf" || resolved == "+inf")
        return "INFINITY";
    if (resolved == "-inf")
        return "-INFINITY";
    return resolved;
}

static bool ternaryBranchesLookScalar(const LowerContext &ctx, const std::string &value) {
    std::string then_branch;
    std::string else_branch;
    if (!splitTopLevelTernary(value, then_branch, else_branch))
        return false;

    auto branch_is_scalar = [&](const std::string &branch) {
        MSLType branch_type = typeForResolvedExpression(ctx, branch);
        if (DXILIRBuilder::isVectorType(branch_type))
            return false;
        if (isUsableMSLType(branch_type))
            return true;
        if (exprLooksScalarLiteral(branch) ||
            exprLooksScalarMathCall(branch) ||
            exprLooksScalarCast(branch))
            return true;
        return !exprLooksVectorValue(branch);
    };

    return branch_is_scalar(then_branch) && branch_is_scalar(else_branch);
}

static std::string dropInvalidScalarComponentAccess(const LowerContext &ctx,
                                                    const std::string &value,
                                                    const MSLType &target) {
    if (!(DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target) ||
          target.kind == MSLTypeKind::Bool) ||
        DXILIRBuilder::isVectorType(target))
        return value;

    std::string base;
    if (!splitTrailingComponentAccess(value, base))
        return value;

    MSLType base_type = typeForResolvedExpression(ctx, base);
    if ((isUsableMSLType(base_type) && !DXILIRBuilder::isVectorType(base_type)) ||
        exprLooksScalarCast(base) ||
        exprLooksScalarMathCall(base) ||
        exprLooksBoolValue(base) ||
        ternaryBranchesLookScalar(ctx, base))
        return base;

    return value;
}

static std::string scalarizeVectorOperands(const LowerContext &ctx, const std::string &expr);
static bool exprLooksScalarizedArithmetic(const std::string &value);

static std::string coerceResolvedValue(const LowerContext &ctx, const std::string &value,
                                       const MSLType &target) {
    std::string sanitized_value = sanitizeThreadVectorCasts(value);
    if (target.kind == MSLTypeKind::Bool) {
        std::string coerced_bool = coerceVectorBoolExpressionForAssignment(ctx, sanitized_value);
        if (!coerced_bool.empty())
            return coerced_bool;
    }

    std::string component_base;
    if ((DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) &&
        !DXILIRBuilder::isVectorType(target) &&
        splitTrailingComponentAccess(sanitized_value, component_base)) {
        MSLType base_type = typeForResolvedValueName(ctx, component_base);
        if ((isUsableMSLType(base_type) && !DXILIRBuilder::isVectorType(base_type)) ||
            (!isUsableMSLType(base_type) &&
             (exprLooksScalarLiteral(component_base) ||
              exprLooksScalarMathCall(component_base) ||
              exprLooksScalarCast(component_base) ||
              ternaryBranchesLookScalar(ctx, component_base)))) {
            return coerceResolvedValue(ctx, component_base, target);
        }
    }

    MSLType source_type = typeForResolvedExpression(ctx, sanitized_value);

    if ((DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) &&
        !DXILIRBuilder::isVectorType(target) &&
        DXILIRBuilder::isVectorType(source_type))
        return "(" + normalizeVectorConstructorArities(sanitized_value) + ").x";

    if ((DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) &&
        !DXILIRBuilder::isVectorType(target) &&
        exprContainsPointerTypedValue(ctx, sanitized_value) &&
        sanitized_value.find("*(") == std::string::npos &&
        sanitized_value.find("reinterpret_cast") == std::string::npos &&
        sanitized_value.find('[') == std::string::npos)
        return defaultForType(target);

    if ((DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) &&
        typeLooksResourceHandle(source_type) &&
        sanitized_value.find("*reinterpret_cast<") == std::string::npos &&
        sanitized_value.find("*reinterpret_cast <") == std::string::npos)
        return defaultForType(target);

    if ((DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) &&
        !DXILIRBuilder::isVectorType(target) &&
        DXILIRBuilder::isVectorType(source_type)) {
        std::string scalarized = scalarizeVectorOperands(ctx, sanitized_value);
        if (scalarized != sanitized_value)
            return coerceResolvedValue(scalarized, target);
        if (exprLooksScalarizedArithmetic(sanitized_value))
            return sanitized_value;
        return coerceResolvedValue("(" + normalizeVectorConstructorArities(sanitized_value) + ").x", target);
    }

    if ((DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) &&
        !DXILIRBuilder::isVectorType(target) &&
        exprLooksScalarizedArithmetic(sanitized_value))
        return sanitized_value;

    if ((DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) &&
        !DXILIRBuilder::isVectorType(target) &&
        sanitized_value.find('?') != std::string::npos &&
        exprLooksVectorValue(sanitized_value))
        return "(" + normalizeVectorConstructorArities(sanitized_value) + ").x";

    std::string then_branch;
    std::string else_branch;
    if ((DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) &&
        !DXILIRBuilder::isVectorType(target) &&
        splitTopLevelTernary(sanitized_value, then_branch, else_branch) &&
        (exprLooksVectorValue(then_branch) || exprLooksVectorValue(else_branch) ||
         DXILIRBuilder::isVectorType(typeForResolvedExpression(ctx, then_branch)) ||
         DXILIRBuilder::isVectorType(typeForResolvedExpression(ctx, else_branch))))
        return "(" + normalizeVectorConstructorArities(sanitized_value) + ").x";

    if ((DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) &&
        !DXILIRBuilder::isVectorType(target) &&
        ternaryBranchesLookScalar(ctx, sanitized_value))
        return sanitized_value;

    return coerceResolvedValue(sanitized_value, target);
}

static std::string hardenGeneratedBoolVectorAssignments(const std::string &source) {
    std::set<std::string> bool_names;
    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(source);
    while (std::getline(in, line)) {
        std::string trimmed = line;
        while (!trimmed.empty() && std::isspace((unsigned char)trimmed.front()))
            trimmed.erase(trimmed.begin());
        if (startsWith(trimmed, "bool v")) {
            size_t name_start = 5;
            size_t name_end = name_start;
            while (name_end < trimmed.size() &&
                   (std::isalnum((unsigned char)trimmed[name_end]) || trimmed[name_end] == '_'))
                name_end++;
            if (name_end > name_start)
                bool_names.insert(trimmed.substr(name_start, name_end - name_start));
        }
        lines.push_back(line);
    }

    std::ostringstream out;
    for (const auto &original : lines) {
        std::string trimmed = original;
        size_t leading = 0;
        while (leading < trimmed.size() && std::isspace((unsigned char)trimmed[leading]))
            leading++;
        trimmed.erase(0, leading);

        size_t eq = trimmed.find(" = ");
        if (eq != std::string::npos && !startsWith(trimmed, "bool ")) {
            std::string name = trimmed.substr(0, eq);
            std::string rhs = trimmed.substr(eq + 3);
            if (!rhs.empty() && rhs.back() == ';')
                rhs.pop_back();

            if (bool_names.find(name) != bool_names.end() &&
                !startsWith(stripEnclosingParens(rhs), "any(") &&
                exprHasTopLevelComparison(rhs) &&
                exprLooksVectorValue(rhs)) {
                std::string stripped_rhs = stripEnclosingParens(rhs);
                bool bitwise_vector_test = (stripped_rhs.find(" & ") != std::string::npos ||
                                            stripped_rhs.find(" | ") != std::string::npos ||
                                            stripped_rhs.find(" ^ ") != std::string::npos) &&
                                           stripped_rhs.size() > 5 &&
                                           stripped_rhs.rfind(" != 0") == stripped_rhs.size() - 5;
                if (bitwise_vector_test) {
                    std::string base = stripped_rhs.substr(0, stripped_rhs.size() - 5);
                    for (const auto &bool_name : bool_names) {
                        std::string component = "(" + bool_name + ").x";
                        std::string promoted = "int4(" + bool_name + ")";
                        size_t pos = 0;
                        while ((pos = base.find(component, pos)) != std::string::npos) {
                            base.replace(pos, component.size(), promoted);
                            pos += promoted.size();
                        }
                    }
                    std::string zero = vectorZeroForExpression(base);
                    if (zero.empty())
                        zero = "int4(0)";
                    out << original.substr(0, leading) << name << " = any(("
                        << base << ") != " << zero << ");\n";
                } else {
                    out << original.substr(0, leading) << name << " = any((" << rhs << "));\n";
                }
                continue;
            }
        }
        out << original << "\n";
    }
    return out.str();
}

static std::string stripEnclosingParens(std::string value) {
    auto trim = [](std::string &text) {
        while (!text.empty() && std::isspace((unsigned char)text.front()))
            text.erase(text.begin());
        while (!text.empty() && std::isspace((unsigned char)text.back()))
            text.pop_back();
    };

    trim(value);
    bool changed = true;
    while (changed && value.size() >= 2 && value.front() == '(' && value.back() == ')') {
        changed = false;
        int depth = 0;
        bool encloses_all = true;
        for (size_t i = 0; i < value.size(); i++) {
            if (value[i] == '(') depth++;
            else if (value[i] == ')') {
                depth--;
                if (depth == 0 && i + 1 != value.size()) {
                    encloses_all = false;
                    break;
                }
            }
            if (depth < 0) {
                encloses_all = false;
                break;
            }
        }
        if (encloses_all && depth == 0) {
            value = value.substr(1, value.size() - 2);
            trim(value);
            changed = true;
        }
    }
    return value;
}

static bool exprEndsWithComponent(const std::string &value) {
    std::string stripped = stripEnclosingParens(value);
    if (stripped.size() < 2) return false;
    char c = stripped.back();
    if (c != 'x' && c != 'y' && c != 'z' && c != 'w' &&
        c != 'r' && c != 'g' && c != 'b' && c != 'a')
        return false;
    return stripped[stripped.size() - 2] == '.';
}

static std::string componentAccess(const std::string &value, uint32_t component, const MSLType &source_type) {
    std::string stripped = stripEnclosingParens(value);
    if (exprLooksScalarCast(stripped) || exprLooksScalarMathCall(stripped))
        return value;
    size_t ternary = stripped.find('?');
    if (ternary != std::string::npos &&
        !exprContainsVectorConstructor(stripped.substr(ternary)))
        return value;
    if (!DXILIRBuilder::isVectorType(source_type) || exprEndsWithComponent(value))
        return value;
    uint32_t value_id = 0;
    if (!parseEmittedValueName(stripEnclosingParens(value), value_id) &&
        !exprLooksVectorValue(value))
        return value;
    std::string normalized = normalizeVectorConstructorArities(value);
    return "(" + normalized + ")" + componentSuffix(component);
}

static std::string coerceVectorWidth(const std::string &value, const MSLType &source_type,
                                     const MSLType &target_type) {
    if (!DXILIRBuilder::isVectorType(source_type) || !DXILIRBuilder::isVectorType(target_type))
        return value;
    std::string target_name = emitTypeName(target_type);
    if (target_name.empty() || target_name == "auto")
        return value;

    uint32_t target_width = DXILIRBuilder::vectorWidth(target_type);
    uint32_t source_width = DXILIRBuilder::vectorWidth(source_type);
    MSLType scalar = DXILIRBuilder::scalarType(target_type);
    std::string zero = defaultForType(scalar);

    std::ostringstream expr;
    expr << target_name << "(";
    for (uint32_t i = 0; i < target_width; i++) {
        if (i) expr << ", ";
        if (i < source_width)
            expr << componentAccess(value, i, source_type);
        else
            expr << zero;
    }
    expr << ")";
    return expr.str();
}

static std::string textureCoordComponent(LowerContext &ctx,
                                         const std::string &value,
                                         uint32_t component) {
    std::string sanitized = sanitizeThreadVectorCasts(value);
    MSLType source_type = typeForResolvedExpression(ctx, sanitized);
    if (typeLooksResourceHandle(source_type) || exprLooksResourceHandle(sanitized) ||
        exprContainsPointerSyntax(sanitized) || exprContainsPointerTypedValue(ctx, sanitized))
        return "0u";
    if (DXILIRBuilder::isVectorType(source_type))
        sanitized = componentAccess(sanitized, component, source_type);
    else if (exprLooksVectorValue(sanitized))
        sanitized = "(" + sanitized + ")" + componentSuffix(component);
    return "static_cast<uint>(" + ensureScalarIndex(sanitized) + ")";
}

static std::string sampleCoordComponent(LowerContext &ctx,
                                        const std::string &value,
                                        uint32_t component) {
    std::string sanitized = sanitizeThreadVectorCasts(value);
    MSLType source_type = typeForResolvedExpression(ctx, sanitized);
    if (typeLooksResourceHandle(source_type) || exprLooksResourceHandle(sanitized) ||
        exprContainsPointerSyntax(sanitized) || exprContainsPointerTypedValue(ctx, sanitized))
        return "0.0f";
    if (DXILIRBuilder::isVectorType(source_type))
        sanitized = componentAccess(sanitized, component, source_type);
    else if (exprLooksVectorValue(sanitized))
        sanitized = "(" + sanitized + ")" + componentSuffix(component);
    return ensureScalarIndex(sanitized);
}

static std::string scalarizeVectorOperands(const LowerContext &ctx, const std::string &expr) {
    std::string out;
    for (size_t i = 0; i < expr.size();) {
        bool token_start = expr[i] == 'v' &&
            (i == 0 || (!std::isalnum((unsigned char)expr[i - 1]) && expr[i - 1] != '_'));
        if (!token_start) {
            out += expr[i++];
            continue;
        }

        size_t end = i + 1;
        while (end < expr.size() && std::isdigit((unsigned char)expr[end]))
            end++;
        if (end == i + 1 ||
            (end < expr.size() && (std::isalnum((unsigned char)expr[end]) || expr[end] == '_'))) {
            out += expr[i++];
            continue;
        }

        std::string name = expr.substr(i, end - i);
        MSLType type = typeForResolvedValueName(ctx, name);
        if (DXILIRBuilder::isVectorType(type)) {
            size_t lookahead = end;
            while (lookahead < expr.size() && std::isspace((unsigned char)expr[lookahead]))
                lookahead++;
            size_t wrapped = lookahead;
            while (wrapped < expr.size() && expr[wrapped] == ')')
                wrapped++;
            while (wrapped < expr.size() && std::isspace((unsigned char)expr[wrapped]))
                wrapped++;
            if ((lookahead >= expr.size() || expr[lookahead] != '.') &&
                (wrapped >= expr.size() || expr[wrapped] != '.'))
                out += "(" + name + ").x";
            else
                out += name;
        } else {
            out += name;
        }
        i = end;
    }
    return out;
}

static bool exprLooksScalarizedArithmetic(const std::string &value) {
    std::string stripped = stripEnclosingParens(value);
    if (exprContainsVectorConstructor(stripped) ||
        stripped.find(".read(") != std::string::npos ||
        stripped.find(".sample(") != std::string::npos ||
        stripped.find(".gather(") != std::string::npos ||
        stripped.find(".gather_compare(") != std::string::npos ||
        stripped.find("reinterpret_cast<device float4&>") != std::string::npos ||
        stripped.find("reinterpret_cast<device uint4&>") != std::string::npos ||
        stripped.find("reinterpret_cast<device int4&>") != std::string::npos)
        return false;
    if (stripped.find(").x") == std::string::npos &&
        stripped.find(").y") == std::string::npos &&
        stripped.find(").z") == std::string::npos &&
        stripped.find(").w") == std::string::npos)
        return false;
    return stripped.find(" + ") != std::string::npos ||
           stripped.find(" - ") != std::string::npos ||
           stripped.find("*") != std::string::npos ||
           stripped.find("/") != std::string::npos;
}

static std::string resolveCondition(LowerContext &ctx, uint32_t idx) {
    std::string value = resolveValue(ctx, idx);
    if (exprLooksResourceHandle(value) || exprContainsRawResourceHandle(value) ||
        exprLooksSideEffectOnly(value))
        return "false";
    std::string stripped_value = stripEnclosingParens(value);
    if (startsWith(stripped_value, "int2(")) return "any(" + value + " != int2(0))";
    if (startsWith(stripped_value, "int3(")) return "any(" + value + " != int3(0))";
    if (startsWith(stripped_value, "int4(")) return "any(" + value + " != int4(0))";
    if (startsWith(stripped_value, "uint2(")) return "any(" + value + " != uint2(0))";
    if (startsWith(stripped_value, "uint3(")) return "any(" + value + " != uint3(0))";
    if (startsWith(stripped_value, "uint4(")) return "any(" + value + " != uint4(0))";
    if (startsWith(stripped_value, "float2(")) return "any(" + value + " != float2(0.0f))";
    if (startsWith(stripped_value, "float3(")) return "any(" + value + " != float3(0.0f))";
    if (startsWith(stripped_value, "float4(")) return "any(" + value + " != float4(0.0f))";
    MSLType condition_type = idx < ctx.value_types.size() ? ctx.value_types[idx] : MSLType{};
    MSLType resolved_type = typeForResolvedExpression(ctx, value);
    if (isUsableMSLType(resolved_type))
        condition_type = resolved_type;
    if (typeLooksResourceHandle(condition_type))
        return "false";
    if (DXILIRBuilder::isVectorType(condition_type)) {
        std::string zero = vectorZeroForExpression(value);
        if (zero.empty())
            zero = defaultForType(condition_type);
        return "any((" + value + ") != " + zero + ")";
    }
    if (DXILIRBuilder::isFloatType(condition_type) || DXILIRBuilder::isIntType(condition_type))
        return "(" + value + " != " + defaultForType(condition_type) + ")";
    return value;
}

static bool usableType(const MSLType &t) {
    return isUsableMSLType(t);
}

static MSLType valueTypeOrUnknown(const LowerContext &ctx, uint32_t idx) {
    if (idx < ctx.value_types.size()) return ctx.value_types[idx];
    return {};
}

static bool hasConstantValue(const LowerContext &ctx, uint32_t idx) {
    for (auto &c : ctx.mod.constants)
        if (c.id == idx && !c.constant_data.empty())
            return true;
    if (ctx.current_fn) {
        for (auto &c : ctx.current_fn->constants)
            if (c.id == idx && !c.constant_data.empty())
                return true;
    }
    return false;
}

static bool hasEmittableValue(const LowerContext &ctx, uint32_t idx) {
    if (idx < ctx.value_table.size() && !ctx.value_table[idx].empty() &&
        !startsWith(ctx.value_table[idx], "dx."))
        return true;
    return hasConstantValue(ctx, idx);
}

static bool isPointerMSLType(const MSLType &t) {
    return t.kind == MSLTypeKind::DeviceCharPtr || t.kind == MSLTypeKind::ThreadgroupCharPtr;
}

static std::vector<uint32_t> functionParamTypeIds(const LLVMModule &module, const LLVMFunction &fn) {
    uint32_t type_id = fn.type_id;
    if (type_id < module.types.size() && module.types[type_id].kind == LLVMType::Pointer &&
        !module.types[type_id].type_refs.empty())
        type_id = module.types[type_id].type_refs[0];
    if (type_id >= module.types.size() || module.types[type_id].kind != LLVMType::Function ||
        module.types[type_id].type_refs.size() <= 1)
        return {};
    std::vector<uint32_t> params;
    params.reserve(module.types[type_id].type_refs.size() - 1);
    for (size_t i = 1; i < module.types[type_id].type_refs.size(); i++)
        params.push_back(module.types[type_id].type_refs[i]);
    return params;
}

static MSLType promoteNumericType(const MSLType &a, const MSLType &b, MSLType fallback) {
    if (!typeLooksResourceHandle(a) && DXILIRBuilder::isVectorType(a)) return a;
    if (!typeLooksResourceHandle(b) && DXILIRBuilder::isVectorType(b)) return b;
    if (!typeLooksResourceHandle(a) && DXILIRBuilder::isFloatType(a)) return a;
    if (!typeLooksResourceHandle(b) && DXILIRBuilder::isFloatType(b)) return b;
    if (!typeLooksResourceHandle(a) && usableType(a)) return a;
    if (!typeLooksResourceHandle(b) && usableType(b)) return b;
    return fallback;
}

static uint32_t literalFromValue(const LowerContext &ctx, uint32_t idx, uint32_t fallback) {
    std::string text;
    if (idx < ctx.value_table.size() && !ctx.value_table[idx].empty())
        text = ctx.value_table[idx];
    else {
        for (auto &c : ctx.mod.constants)
            if (c.id == idx && !c.constant_data.empty()) { text = c.constant_data; break; }
        if (text.empty() && ctx.current_fn)
            for (auto &c : ctx.current_fn->constants)
                if (c.id == idx && !c.constant_data.empty()) { text = c.constant_data; break; }
    }
    uint32_t value = 0;
    if (parseUnsignedLiteral(text, value)) return value;
    return fallback;
}

static bool valueIsUnresolvedTemp(const LowerContext &ctx, uint32_t idx) {
    if (hasConstantValue(ctx, idx))
        return false;
    if (idx >= ctx.value_table.size())
        return true;
    return ctx.value_table[idx].empty() || ctx.value_table[idx] == emitValue(idx);
}

static bool hasVertexInputForRegister(const LowerContext &ctx, uint32_t shader_register) {
    for (const auto &input : ctx.options.vertex_inputs) {
        if (!input.system_value && input.shader_register == shader_register)
            return true;
    }
    return false;
}

static bool isLoadInputI32(const std::string &callee_name) {
    return callee_name.find("loadInput.i32") != std::string::npos;
}

static bool isLoadInputF32(const std::string &callee_name) {
    return callee_name.find("loadInput.f32") != std::string::npos;
}

static bool shouldLowerLoadInputI32AsVertexId(const LowerContext &ctx, uint32_t input_id) {
    return ctx.shader.kind == DxilShaderKind::Vertex &&
           input_id == 0 &&
           !ctx.vertex_has_float_load_input &&
           ctx.vertex_input_ids.size() == 1 &&
           ctx.vertex_input_ids.count(0) != 0;
}

static bool shouldLowerArgumentlessLoadInputI32AsVertexId(const LowerContext &ctx) {
    return ctx.shader.kind == DxilShaderKind::Vertex &&
           ctx.options.vertex_inputs.empty() &&
           !ctx.vertex_has_float_load_input &&
           ctx.vertex_input_ids.empty();
}

static bool hasArgumentlessLoadInputF32(const LowerContext &ctx,
                                        const LLVMFunction &fn) {
    for (const auto &block : fn.blocks) {
        for (const auto &inst : block.instructions) {
            if (inst.opcode != LLVMInstruction::Call || inst.operands.empty())
                continue;

            std::string callee_name;
            auto decl_it = ctx.function_decls.find(inst.operands[0]);
            if (decl_it != ctx.function_decls.end())
                callee_name = decl_it->second;
            else if (inst.operands[0] < ctx.value_table.size())
                callee_name = ctx.value_table[inst.operands[0]];

            if (isLoadInputF32(callee_name) && inst.operands.size() <= 2)
                return true;
        }
    }
    return false;
}

static bool hasArgumentlessLoadInputF32Declaration(const LLVMModule &module) {
    for (const auto &fn : module.functions) {
        if (!fn.is_declaration || fn.name.empty())
            continue;
        if (isLoadInputF32(fn.name) && fn.param_count == 0)
            return true;
    }
    return false;
}

static DescriptorRangePlan::Kind descriptorKindForResourceClass(uint32_t resource_class) {
    switch (resource_class) {
    case 0: return DescriptorRangePlan::Kind::SRV;
    case 1: return DescriptorRangePlan::Kind::UAV;
    case 2: return DescriptorRangePlan::Kind::CBV;
    case 3: return DescriptorRangePlan::Kind::Sampler;
    default: return DescriptorRangePlan::Kind::SRV;
    }
}

static const char *bindingPrefixForKind(DescriptorRangePlan::Kind kind) {
    switch (kind) {
    case DescriptorRangePlan::Kind::CBV: return "buf";
    case DescriptorRangePlan::Kind::Sampler: return "samp";
    case DescriptorRangePlan::Kind::SRV:
    case DescriptorRangePlan::Kind::UAV:
        return "tex";
    }
    return "buf";
}

static MSLType typeForHandleKind(DescriptorRangePlan::Kind kind) {
    switch (kind) {
    case DescriptorRangePlan::Kind::CBV:
        return {MSLTypeKind::DeviceCharPtr, 0, {}};
    case DescriptorRangePlan::Kind::Sampler:
        return {MSLTypeKind::Sampler, 0, {}};
    case DescriptorRangePlan::Kind::SRV:
        return {MSLTypeKind::Texture2D, 0, {}};
    case DescriptorRangePlan::Kind::UAV:
        return {MSLTypeKind::RWTexture2D, 0, {}};
    }
    return {MSLTypeKind::DeviceCharPtr, 0, {}};
}

static MSLType typeForHandleKind(const LowerContext &ctx, DescriptorRangePlan::Kind kind) {
    MSLType type = typeForHandleKind(kind);
    if (kind == DescriptorRangePlan::Kind::UAV &&
        ctx.texture_store_sample_shader)
        return {MSLTypeKind::RWTexture2DArray, 0, {}};
    if (ctx.shader.kind == DxilShaderKind::Compute &&
        (ctx.compute_texture_store_shader ||
         !ctx.compute_texture_sample_shader) &&
        type.kind == MSLTypeKind::Texture2D)
        return {MSLTypeKind::RWTexture2D, 0, {}};
    return type;
}

static bool isWritableMSAAHandle(const LowerContext &ctx, uint32_t handle_id) {
    auto it = ctx.resource_handles.find(handle_id);
    return it != ctx.resource_handles.end() &&
           it->second.kind == DescriptorRangePlan::Kind::UAV &&
           (it->second.resource_kind == 3u ||
            it->second.resource_kind == 8u);
}

static bool isWritableMSAAArrayHandle(const LowerContext &ctx,
                                      uint32_t handle_id) {
    auto it = ctx.resource_handles.find(handle_id);
    return isWritableMSAAHandle(ctx, handle_id) &&
           it != ctx.resource_handles.end() &&
           it->second.resource_kind == 8u;
}

static uint32_t writableMSAASampleCount(const LowerContext &ctx,
                                        uint32_t handle_id) {
    auto it = ctx.resource_handles.find(handle_id);
    if (it == ctx.resource_handles.end() || it->second.sample_count == 0)
        return 1;
    return it->second.sample_count;
}

static std::string writableMSAASlice(const LowerContext &ctx,
                                     uint32_t handle_id,
                                     const std::string &sample,
                                     const std::string &array_slice) {
    if (!isWritableMSAAArrayHandle(ctx, handle_id))
        return sample;
    return "((uint)(" + array_slice + ") * " +
           std::to_string(writableMSAASampleCount(ctx, handle_id)) +
           "u + (uint)(" + sample + "))";
}

static MSLType typeForResourceHandle(const LowerContext &ctx,
                                     const ResourceHandleRecord &handle) {
    if (isAccelerationStructureResourceKind(handle.resource_kind))
        return {MSLTypeKind::InstanceAccelerationStructure, 0, {}};
    if (handle.kind == DescriptorRangePlan::Kind::UAV) {
        // Writable MSAA textures use the existing flattened texture2d_array
        // ABI.  The sample index is folded into the array slice by the
        // intrinsic emitter.
        if (isTextureMSAAResourceKind(handle.resource_kind))
            return {MSLTypeKind::RWTexture2DArray, 0, {}};
        if (isTexture3DResourceKind(handle.resource_kind))
            return {MSLTypeKind::RWTexture3D, 0, {}};
        if (isTextureArrayResourceKind(handle.resource_kind))
            return {MSLTypeKind::RWTexture2DArray, 0, {}};
        return typeForHandleKind(ctx, handle.kind);
    }
    if (handle.kind == DescriptorRangePlan::Kind::SRV) {
        if (isTextureMSAAResourceKind(handle.resource_kind))
            return {MSLTypeKind::Texture2DMS, 0, {}};
        if (isTexture3DResourceKind(handle.resource_kind))
            return {MSLTypeKind::Texture3D, 0, {}};
        if (isTextureCubeResourceKind(handle.resource_kind))
            return {MSLTypeKind::TextureCube, 0, {}};
        if (handle.resource_kind == 6u || handle.resource_kind == 7u)
            return {MSLTypeKind::Texture2DArray, 0, {}};
    }
    return typeForHandleKind(ctx, handle.kind);
}

static ValueRole roleForHandleKind(DescriptorRangePlan::Kind kind) {
    switch (kind) {
    case DescriptorRangePlan::Kind::CBV:
        return ValueRole::BufferHandle;
    case DescriptorRangePlan::Kind::Sampler:
        return ValueRole::SamplerHandle;
    case DescriptorRangePlan::Kind::SRV:
    case DescriptorRangePlan::Kind::UAV:
        return ValueRole::TextureHandle;
    }
    return ValueRole::Generic;
}

static uint32_t directBufferBindingIndex(const LowerContext &ctx,
                                         const ResourceHandleRecord &handle,
                                         const char *target_prefix) {
    uint32_t binding_index = handle.lower_bound + handle.binding_index;
    // D3D12 keeps SRV, UAV, and CBV register namespaces independent.  The
    // direct MSL ABI has one buffer namespace, so reserve 0..7 for UAVs,
    // 8..15 for CBVs, and 16..30 for SRVs.  Textures retain their texture
    // namespace and are unaffected.
    if (std::strcmp(target_prefix, "buf") == 0 ||
        std::strcmp(target_prefix, "as") == 0) {
        if (handle.kind == DescriptorRangePlan::Kind::SRV)
            binding_index += 16;
        else if (handle.kind == DescriptorRangePlan::Kind::CBV)
            binding_index += 8;
    }
    return binding_index;
}

static uint32_t cappedBindingIndex(const LowerContext &ctx, const char *prefix, uint32_t binding_index) {
    uint32_t limit = 0;
    if (std::strcmp(prefix, "buf") == 0 || std::strcmp(prefix, "as") == 0)
        limit = ctx.binding_plan.direct_buffer_count;
    else if (std::strcmp(prefix, "tex") == 0)
        limit = ctx.binding_plan.direct_texture_count;
    else if (std::strcmp(prefix, "samp") == 0)
        limit = ctx.binding_plan.direct_sampler_count;

    if (limit == 0)
        return 0;
    return std::min<uint32_t>(binding_index, limit - 1);
}

static std::string materializeHandleName(const LowerContext &ctx,
                                         const ResourceHandleRecord &handle,
                                         const char *target_prefix = nullptr) {
    const char *prefix = target_prefix
                             ? target_prefix
                             : (isAccelerationStructureResourceKind(
                                    handle.resource_kind)
                                    ? "as"
                                    : bindingPrefixForKind(handle.kind));
    const bool direct_buffer_binding =
        std::strcmp(prefix, "buf") == 0 || std::strcmp(prefix, "as") == 0;
    uint32_t binding_index = direct_buffer_binding
                                 ? directBufferBindingIndex(ctx, handle, prefix)
                                 : handle.lower_bound + handle.binding_index;
    if (target_prefix && std::strcmp(target_prefix, "buf") == 0 &&
        (handle.kind == DescriptorRangePlan::Kind::SRV ||
         handle.kind == DescriptorRangePlan::Kind::UAV) &&
        !handle.dynamic_index.empty() && handle.binding_count > 1) {
        // The direct MSL ABI exposes one pointer per buffer slot rather than
        // an argument-buffer array. Preserve a dynamically indexed SRV/UAV
        // binding with a bounded pointer-select expression instead of
        // collapsing it to element zero. Loads are subsequently expanded to
        // typed helpers; stores can write through the selected device pointer.
        std::string selected = "buf" + std::to_string(cappedBindingIndex(
            ctx, prefix, binding_index + handle.binding_count - 1));
        for (uint32_t i = handle.binding_count - 1; i > 0; --i) {
            uint32_t candidate = cappedBindingIndex(ctx, prefix, binding_index + i - 1);
            selected = "((uint(" + handle.dynamic_index + ") == " +
                       std::to_string(i - 1) + "u) ? buf" +
                       std::to_string(candidate) + " : " + selected + ")";
        }
        return selected;
    }
    return std::string(prefix) + std::to_string(cappedBindingIndex(ctx, prefix, binding_index));
}

static uint32_t resourceKindForHandle(const LowerContext &ctx, uint32_t handle_id) {
    auto it = ctx.resource_handles.find(handle_id);
    return it == ctx.resource_handles.end() ? 0u : it->second.resource_kind;
}

static void recordDescriptorRange(BindingPlan &plan, DescriptorRangePlan range) {
    if (range.count == 0)
        range.count = 1;
    for (auto &existing : plan.ranges) {
        if (existing.kind == range.kind &&
            existing.register_space == range.register_space &&
            existing.lower_bound == range.lower_bound) {
            existing.count = std::max(existing.count, range.count);
            if (existing.resource_kind == 0)
                existing.resource_kind = range.resource_kind;
            if (existing.element_type == 0)
                existing.element_type = range.element_type;
            if (existing.element_stride == 0)
                existing.element_stride = range.element_stride;
            if (existing.sample_count <= 1)
                existing.sample_count = range.sample_count;
            return;
        }
    }
    plan.ranges.push_back(range);
}

static const DxilResourceBinding *findResourceBinding(
    const LLVMModule &module, uint32_t resource_class, uint32_t resource_id) {
    for (const auto &binding : module.resource_bindings) {
        if (binding.resource_class == resource_class &&
            binding.resource_id == resource_id)
            return &binding;
    }
    return nullptr;
}

static const DxilResourceBinding *findResourceBindingAtBinding(
    const LLVMModule &module, uint32_t resource_class,
    uint32_t register_space, uint32_t lower_bound) {
    for (const auto &binding : module.resource_bindings) {
        if (binding.resource_class == resource_class &&
            binding.register_space == register_space &&
            binding.lower_bound == lower_bound)
            return &binding;
    }
    return nullptr;
}

static void applyResourceBindingMetadata(
    const DxilResourceBinding *metadata, ResourceHandleRecord &record) {
    if (!metadata)
        return;
    record.resource_class = metadata->resource_class;
    record.register_space = metadata->register_space;
    record.lower_bound = metadata->lower_bound;
    record.binding_count = metadata->count ? metadata->count : 1;
    record.resource_kind = metadata->resource_kind;
    record.element_type = metadata->element_type;
    record.element_stride = metadata->element_stride;
    record.sample_count = metadata->sample_count ? metadata->sample_count : 1;
}

static void analyzeBindingPlan(LowerContext &ctx, const LLVMFunction &fn) {
    BindingPlan plan;
    struct HandleBinding {
        DescriptorRangePlan::Kind kind = DescriptorRangePlan::Kind::SRV;
        uint32_t lower_bound = 0;
        uint32_t count = 1;
        std::string dynamic_index;
        bool direct_heap = false;
    };
    std::unordered_map<uint32_t, HandleBinding> handle_bindings;
    auto rememberHandle = [&](uint32_t result_id,
                              DescriptorRangePlan::Kind kind,
                              uint32_t lower_bound, uint32_t count,
                              uint32_t binding_index = 0,
                              const DxilResourceBinding *metadata = nullptr,
                              const std::string &dynamic_index = {},
                              bool direct_heap = false) {
        if (!result_id)
            return;
        handle_bindings[result_id] = {kind, lower_bound, count, dynamic_index,
                                      direct_heap};
        ResourceHandleRecord record;
        record.kind = kind;
        record.lower_bound = lower_bound;
        record.binding_count = count ? count : 1;
        record.binding_index = binding_index;
        record.dynamic_index = dynamic_index;
        record.direct_heap = direct_heap;
        applyResourceBindingMetadata(metadata, record);
        ctx.resource_handles[result_id] = record;
    };
    auto markWritableMSAASlots = [&](uint32_t handle_id) {
        auto it = handle_bindings.find(handle_id);
        if (it == handle_bindings.end() ||
            it->second.kind != DescriptorRangePlan::Kind::UAV)
            return;
        uint32_t count = std::min<uint32_t>(it->second.count, 16);
        for (uint32_t i = 0; i < count; ++i)
            ctx.writable_msaa_texture_slots.insert(it->second.lower_bound + i);
    };
    auto producesValue = [&](const LLVMInstruction &inst) {
        switch (inst.opcode) {
        case LLVMInstruction::Ret:
        case LLVMInstruction::Br:
        case LLVMInstruction::Switch:
        case LLVMInstruction::Unreachable:
        case LLVMInstruction::Store:
            return false;
        case LLVMInstruction::Call:
            return inst.type_id < ctx.mod.types.size() &&
                   ctx.mod.types[inst.type_id].kind != LLVMType::Void;
        default:
            return true;
        }
    };
    uint32_t value_counter = fn.instruction_start_value;
    struct IndexRange {
        bool known = false;
        uint32_t lower = 0;
        uint32_t upper = 0;
    };
    std::unordered_map<uint32_t, IndexRange> index_ranges;

    auto calleeName = [&](uint32_t callee) -> std::string {
        auto decl_it = ctx.function_decls.find(callee);
        if (decl_it != ctx.function_decls.end())
            return decl_it->second;
        if (callee < ctx.value_table.size())
            return ctx.value_table[callee];
        return {};
    };
    auto valueText = [&](uint32_t value_id) -> std::string {
        if (value_id < ctx.value_table.size() &&
            !ctx.value_table[value_id].empty())
            return ctx.value_table[value_id];
        for (const auto &constant : ctx.mod.constants)
            if (constant.id == value_id && !constant.constant_data.empty())
                return constant.constant_data;
        for (const auto &constant : fn.constants)
            if (constant.id == value_id && !constant.constant_data.empty())
                return constant.constant_data;
        return emitValue(value_id);
    };
    auto dynamicIndexFor = [&](uint32_t value_id, uint32_t count) -> std::string {
        if (count <= 1)
            return {};
        const std::string text = valueText(value_id);
        uint32_t literal = 0;
        return parseUnsignedLiteral(text, literal) ? std::string() : text;
    };
    auto indexRangeFor = [&](uint32_t value_id) {
        auto known = index_ranges.find(value_id);
        if (known != index_ranges.end())
            return known->second;
        uint32_t literal = 0;
        if (parseUnsignedLiteral(valueText(value_id), literal))
            return IndexRange{true, literal, literal};
        return IndexRange{};
    };
    auto rememberIndexRange = [&](const LLVMInstruction &inst,
                                  uint32_t result_id) {
        if (!result_id || inst.operands.empty())
            return;
        const IndexRange lhs = indexRangeFor(inst.operands[0]);
        IndexRange result;
        if ((inst.opcode == LLVMInstruction::ZExt ||
             inst.opcode == LLVMInstruction::Trunc) && lhs.known) {
            result = lhs;
        } else if (inst.operands.size() < 2) {
            return;
        } else if (inst.opcode == LLVMInstruction::And) {
            const IndexRange rhs = indexRangeFor(inst.operands[1]);
            if (lhs.known && lhs.lower == lhs.upper) {
                result = {true, 0, lhs.upper};
            } else if (rhs.known && rhs.lower == rhs.upper) {
                result = {true, 0, rhs.upper};
            }
        } else if (inst.opcode == LLVMInstruction::Add && lhs.known) {
            const IndexRange rhs = indexRangeFor(inst.operands[1]);
            if (rhs.known) {
                const uint64_t lower = uint64_t(lhs.lower) + rhs.lower;
                const uint64_t upper = uint64_t(lhs.upper) + rhs.upper;
                if (upper <= UINT32_MAX)
                    result = {true, static_cast<uint32_t>(lower),
                              static_cast<uint32_t>(upper)};
            }
        }
        if (result.known)
            index_ranges[result_id] = result;
    };

    for (const auto &block : fn.blocks) {
        for (const auto &inst : block.instructions) {
            if (inst.opcode != LLVMInstruction::Call || inst.operands.size() < 2) {
                if (producesValue(inst)) {
                    rememberIndexRange(inst, value_counter);
                    ++value_counter;
                }
                continue;
            }

            std::string callee_name = calleeName(inst.operands[0]);
            uint32_t intrinsic_id = intrinsicIdFromCalleeName(callee_name);

            std::vector<uint32_t> call_args;
            for (size_t i = 2; i < inst.operands.size(); i++)
                call_args.push_back(inst.operands[i]);

            if (!call_args.empty() && (callee_name.empty() || startsWith(callee_name, "dx.op."))) {
                uint32_t opcode = literalFromValue(ctx, call_args[0], 0);
                if (isOpcodePrefixedDXIntrinsic(opcode))
                    intrinsic_id = canonicalDXIntrinsicId(opcode);
            }
            if (intrinsic_id == DXOP_SpecialFloat && !call_args.empty()) {
                uint32_t opcode = literalFromValue(ctx, call_args[0], 0);
                if (opcode >= 8 && opcode <= 11)
                    intrinsic_id = opcode;
                else
                    intrinsic_id = 0;
            }
            if (intrinsic_id == 0) {
                if (producesValue(inst))
                    ++value_counter;
                continue;
            }

            uint32_t result_id = value_counter;
            std::vector<uint32_t> fn_args;
            if (intrinsic_id == DXOP_CreateHandle || intrinsic_id == DXOP_CreateHandleForLib ||
                intrinsic_id == DXOP_AnnotateHandle) {
                if (call_args.size() > 1)
                    fn_args.assign(call_args.begin() + 1, call_args.end());
            } else if (call_args.size() > 1)
                fn_args.assign(call_args.begin() + 1, call_args.end());

            if (intrinsic_id == DXOP_CreateHandle && fn_args.size() >= 3) {
                uint32_t resource_class = literalFromValue(ctx, fn_args[0], 0);
                uint32_t range_id = literalFromValue(ctx, fn_args[1], 0);
                uint32_t index = literalFromValue(ctx, fn_args[2], 0);
                uint32_t non_uniform_raw = fn_args.size() >= 4 ? literalFromValue(ctx, fn_args[3], 0) : 0;
                if (non_uniform_raw > 1)
                    index = 0;
                auto kind = descriptorKindForResourceClass(resource_class);
                const auto *metadata =
                    findResourceBinding(ctx.mod, resource_class, range_id);
                if (metadata) {
                    recordDescriptorRange(
                        plan, {kind, metadata->register_space,
                               metadata->lower_bound, metadata->count,
                               metadata->resource_kind,
                               metadata->element_type,
                               metadata->element_stride,
                               metadata->sample_count});
                    rememberHandle(
                        result_id, kind, metadata->lower_bound, metadata->count,
                        metadata->count <= 1 ? 0 : index, metadata,
                        fn_args.size() > 2
                            ? dynamicIndexFor(fn_args[2], metadata->count)
                            : std::string());
                } else {
                    recordDescriptorRange(plan, {kind, 0, index, 1});
                    rememberHandle(result_id, kind, 0, 1, index);
                }
            } else if (intrinsic_id == DXOP_CreateHandleFromBinding && fn_args.size() >= 1) {
                std::string binding = resolveValue(ctx, fn_args[0]);
                auto parts = parseAggregateLiteral(binding);
                uint32_t lower_bound = 0, upper_bound = 0, count = 1;
                uint32_t space = 0, resource_class = 0;
                if (parts.size() > 0) parseUnsignedLiteral(parts[0], lower_bound);
                if (parts.size() > 1) parseUnsignedLiteral(parts[1], upper_bound);
                if (parts.size() > 2) parseUnsignedLiteral(parts[2], space);
                if (parts.size() > 3) parseUnsignedLiteral(parts[3], resource_class);
                if (upper_bound >= lower_bound)
                    count = upper_bound - lower_bound + 1;
                auto kind = descriptorKindForResourceClass(resource_class);
                uint32_t index = fn_args.size() > 1
                                    ? literalFromValue(ctx, fn_args[1], 0)
                                    : 0;
                const auto *metadata = findResourceBindingAtBinding(
                    ctx.mod, resource_class, space, lower_bound);
                if (metadata) {
                    recordDescriptorRange(
                        plan, {kind, metadata->register_space,
                               metadata->lower_bound, metadata->count,
                               metadata->resource_kind,
                               metadata->element_type,
                               metadata->element_stride,
                               metadata->sample_count});
                    rememberHandle(
                        result_id, kind, metadata->lower_bound, metadata->count,
                        metadata->count <= 1 ? 0 : index, metadata,
                        fn_args.size() > 1
                            ? dynamicIndexFor(fn_args[1], metadata->count)
                            : std::string());
                } else {
                    recordDescriptorRange(plan, {kind, space, lower_bound, count});
                    rememberHandle(
                        result_id, kind, lower_bound, count, index, nullptr,
                        fn_args.size() > 1
                            ? dynamicIndexFor(fn_args[1], count)
                            : std::string());
                }
            } else if (intrinsic_id == DXOP_CreateHandleFromHeap && fn_args.size() >= 1) {
                uint32_t heap_index = literalFromValue(ctx, fn_args[0], 0);
                bool sampler = fn_args.size() >= 2 && literalFromValue(ctx, fn_args[1], 0) != 0;
                auto kind = sampler ? DescriptorRangePlan::Kind::Sampler
                                     : DescriptorRangePlan::Kind::SRV;
                const std::string heap_value = resolveValue(ctx, fn_args[0]);
                uint32_t literal_heap_index = 0;
                const bool dynamic_heap_index =
                    !parseUnsignedLiteral(heap_value, literal_heap_index);
                if (!dynamic_heap_index)
                    heap_index = literal_heap_index;
                const IndexRange bounded_index = indexRangeFor(fn_args[0]);
                const uint32_t limit =
                    sampler ? std::min<uint32_t>(4u, plan.direct_sampler_count)
                            : std::min<uint32_t>(8u, plan.direct_buffer_count);
                const bool has_bounded_index =
                    dynamic_heap_index && bounded_index.known &&
                    bounded_index.lower <= bounded_index.upper &&
                    bounded_index.upper < limit;
                const uint32_t count =
                    dynamic_heap_index
                        ? (has_bounded_index
                               ? bounded_index.upper - bounded_index.lower + 1
                               : limit)
                        : 1u;
                const uint32_t lower_bound =
                    dynamic_heap_index
                        ? (has_bounded_index ? bounded_index.lower : 0u)
                        : heap_index;
                const std::string dynamic_index =
                    dynamic_heap_index ? heap_value : std::string();
                recordDescriptorRange(plan,
                                      {kind, 0, lower_bound, count});
                rememberHandle(result_id, kind, lower_bound, count, 0,
                               nullptr, dynamic_index, true);
            } else if (intrinsic_id == DXOP_AnnotateHandle &&
                       fn_args.size() >= 2) {
                auto base = handle_bindings.find(fn_args[0]);
                if (base != handle_bindings.end()) {
                    rememberHandle(result_id, base->second.kind,
                                   base->second.lower_bound,
                                   base->second.count, 0, nullptr,
                                   base->second.dynamic_index,
                                   base->second.direct_heap);
                    auto properties = parseAggregateLiteral(
                        resolveValue(ctx, fn_args[1]));
                    auto base_record = ctx.resource_handles.find(fn_args[0]);
                    if (base_record != ctx.resource_handles.end()) {
                        auto annotated = base_record->second;
                        uint32_t property0 = 0;
                        if (!properties.empty() &&
                            parseUnsignedLiteral(properties[0], property0)) {
                            annotated.resource_kind = property0 & 0xffu;
                            if (property0 & 0x1000u) {
                                annotated.kind = DescriptorRangePlan::Kind::UAV;
                                annotated.resource_class = 1;
                            } else if (annotated.kind !=
                                       DescriptorRangePlan::Kind::Sampler) {
                                annotated.kind = DescriptorRangePlan::Kind::SRV;
                                annotated.resource_class = 0;
                            }
                        }
                        if (properties.size() > 1) {
                            uint32_t property1 = 0;
                            if (parseUnsignedLiteral(properties[1], property1)) {
                                annotated.element_stride = property1;
                                if (annotated.resource_kind >= 1u &&
                                    annotated.resource_kind <= 10u) {
                                    annotated.element_type = property1 & 0xffu;
                                    const uint32_t samples =
                                        (property1 >> 16) & 0xffu;
                                    annotated.sample_count =
                                        samples ? samples : 1u;
                                }
                            }
                        }
                        ctx.resource_handles[result_id] = annotated;
                        auto annotated_binding = handle_bindings.find(result_id);
                        if (annotated_binding != handle_bindings.end())
                            annotated_binding->second.kind = annotated.kind;
                        if (annotated.direct_heap) {
                            for (auto &range : plan.ranges) {
                                if (range.lower_bound != annotated.lower_bound ||
                                    range.count != annotated.binding_count)
                                    continue;
                                range.kind = annotated.kind;
                                range.resource_kind =
                                    annotated.resource_kind;
                                range.element_type = annotated.element_type;
                                range.element_stride =
                                    annotated.element_stride;
                                range.sample_count = annotated.sample_count;
                            }
                        }
                    }
                    if (base->second.kind == DescriptorRangePlan::Kind::UAV) {
                        uint32_t resource_kind = 0;
                        if (!properties.empty() &&
                            parseUnsignedLiteral(properties[0], resource_kind) &&
                            ((resource_kind & 0xffu) == 3u ||
                             (resource_kind & 0xffu) == 8u))
                            markWritableMSAASlots(fn_args[0]);
                    }
                }
            }
            if ((intrinsic_id == DXOP_TextureSampleCmp ||
                 intrinsic_id == DXOP_TextureSampleCmpLevelZero ||
                 intrinsic_id == DXOP_TextureSampleCmpLevel ||
                 intrinsic_id == DXOP_TextureSampleCmpGrad ||
                 intrinsic_id == DXOP_TextureSampleCmpBias ||
                 intrinsic_id == DXOP_TextureGatherCmp) &&
                !fn_args.empty()) {
                auto comparison_handle = handle_bindings.find(fn_args[0]);
                if (comparison_handle != handle_bindings.end()) {
                    const uint32_t count =
                        std::min<uint32_t>(comparison_handle->second.count, 16);
                    for (uint32_t i = 0; i < count; ++i)
                        ctx.comparison_texture_slots.insert(
                            comparison_handle->second.lower_bound + i);
                }
            }
            if (producesValue(inst))
                ++value_counter;
        }
    }

    // Some optimized graphics entry points renumber the annotated comparison
    // handle outside the pre-analysis value sequence.  Preserve precise
    // per-slot typing for mixed shaders, but recover the unambiguous case when
    // the entry point exposes exactly one sampled texture range.
    if (ctx.sample_cmp_shader && ctx.comparison_texture_slots.empty()) {
        uint32_t only_slot = 0;
        uint32_t texture_slot_count = 0;
        for (const auto &range : plan.ranges) {
            if (range.kind != DescriptorRangePlan::Kind::SRV ||
                !isTextureResourceKind(range.resource_kind))
                continue;
            texture_slot_count += range.count;
            only_slot = range.lower_bound;
        }
        if (texture_slot_count == 1)
            ctx.comparison_texture_slots.insert(only_slot);
    }

    uint32_t max_sampler = 0;
    uint32_t max_texture = 0;
    bool has_sampler = false;
    bool has_texture = false;
    for (const auto &range : plan.ranges) {
        if (range.kind == DescriptorRangePlan::Kind::Sampler) {
            has_sampler = true;
            max_sampler = std::max(max_sampler, range.lower_bound + range.count);
        } else if ((range.kind == DescriptorRangePlan::Kind::SRV ||
                    range.kind == DescriptorRangePlan::Kind::UAV) &&
                   !isAccelerationStructureResourceKind(range.resource_kind)) {
            has_texture = true;
            max_texture = std::max(max_texture, range.lower_bound + range.count);
        }
    }
    if (has_texture) {
        uint32_t texture_limit = ctx.shader.kind == DxilShaderKind::Compute ? 8 : 16;
        plan.direct_texture_count = std::max<uint32_t>(1, std::min<uint32_t>(max_texture, texture_limit));
        if (ctx.shader.kind != DxilShaderKind::Compute)
            plan.direct_sampler_count = std::max<uint32_t>(plan.direct_sampler_count, 1);
    }
    if (has_sampler)
        plan.direct_sampler_count = std::max<uint32_t>(1, std::min<uint32_t>(max_sampler, 4));
    ctx.binding_plan = std::move(plan);
}

static void analyzeVertexInputs(LowerContext &ctx, const LLVMFunction &fn) {
    if (ctx.shader.kind != DxilShaderKind::Vertex)
        return;

    auto calleeName = [&](uint32_t callee) -> std::string {
        auto decl_it = ctx.function_decls.find(callee);
        if (decl_it != ctx.function_decls.end())
            return decl_it->second;
        if (callee < ctx.value_table.size())
            return ctx.value_table[callee];
        return {};
    };

    for (const auto &block : fn.blocks) {
        for (const auto &inst : block.instructions) {
            if (inst.opcode != LLVMInstruction::Call || inst.operands.size() < 4)
                continue;

            std::string callee_name = calleeName(inst.operands[0]);
            if (isLoadInputF32(callee_name))
                ctx.vertex_has_float_load_input = true;

            uint32_t intrinsic_id = intrinsicIdFromCalleeName(callee_name);
            if (intrinsic_id != DXOP_LoadInput)
                continue;

            std::vector<uint32_t> call_args;
            for (size_t i = 2; i < inst.operands.size(); i++)
                call_args.push_back(inst.operands[i]);
            if (!call_args.empty())
                call_args.erase(call_args.begin());
            if (!call_args.empty()) {
                uint32_t input_id = literalFromValue(ctx, call_args[0], 0);
                if (input_id < 16)
                    ctx.vertex_input_ids.insert(input_id);
            }
        }
    }
}

static MSLType inferDXIntrinsicResultType(LowerContext &ctx, uint32_t intrinsic_id,
                                          const std::vector<uint32_t> &args,
                                          MSLType declared = {},
                                          const std::string &callee_name = {}) {
    switch (intrinsic_id) {
    case DXOP_CreateHandle:
    case DXOP_CreateHandleForLib: {
        uint32_t resource_class = args.empty() ? 0 : literalFromValue(ctx, args[0], 0);
        return typeForHandleKind(ctx, descriptorKindForResourceClass(resource_class));
    }
    case DXOP_CreateHandleFromBinding: {
        if (!args.empty()) {
            std::string binding = resolveValue(ctx, args[0]);
            auto parts = parseAggregateLiteral(binding);
            uint32_t resource_class = 0;
            if (parts.size() > 3)
                parseUnsignedLiteral(parts[3], resource_class);
            return typeForHandleKind(ctx, descriptorKindForResourceClass(resource_class));
        }
        return {MSLTypeKind::DeviceCharPtr, 0, {}};
    }
    case DXOP_CreateHandleFromHeap: {
        bool sampler = args.size() >= 2 && literalFromValue(ctx, args[1], 0) != 0;
        return typeForHandleKind(ctx, sampler ? DescriptorRangePlan::Kind::Sampler
                                              : DescriptorRangePlan::Kind::SRV);
    }
    case DXOP_AllocateRayQuery:
    case DXOP_AllocateRayQuery2:
        return {MSLTypeKind::RayQuery, 0, {}};
    case DXOP_TempRegLoad:
    case DXOP_MinPrecXRegLoad:
        if (usableType(declared))
            return declared;
        if (callee_name.find(".i1") != std::string::npos)
            return {MSLTypeKind::Bool, 0, {}};
        if (callee_name.find(".i32") != std::string::npos)
            return {MSLTypeKind::Int, 0, {}};
        if (callee_name.find(".f16") != std::string::npos)
            return {MSLTypeKind::Half, 0, {}};
        return {MSLTypeKind::Float, 0, {}};
    case DXOP_TempRegStore:
    case DXOP_MinPrecXRegStore:
        return {MSLTypeKind::Void, 0, {}};
    case DXOP_RayQueryProceed:
        return {MSLTypeKind::Bool, 0, {}};
    case DXOP_RayQueryCandidateType:
    case DXOP_RayQueryCommittedStatus:
    case 190:
    case 191:
    case 192:
    case 195:
    case 201:
    case 202:
    case 203:
    case 204:
    case 207:
    case 208:
    case 209:
    case 210:
    case 214:
    case 215:
        return {MSLTypeKind::UInt, 0, {}};
    case 193:
    case 194:
    case 196:
    case 197:
    case 205:
    case 206:
    case 211:
    case 212:
        return {MSLTypeKind::Float, 0, {}};
    case 198:
    case 199:
    case 200:
        return {MSLTypeKind::Float, 0, {}};
    case 186:
    case 187:
    case 188:
    case 189:
        return declared;
    case DXOP_RayQueryTraceRayInline:
    case DXOP_RayQueryAbort:
    case DXOP_RayQueryCommitNonOpaqueTriangleHit:
    case DXOP_RayQueryCommitProceduralPrimitiveHit:
        return {MSLTypeKind::Void, 0, {}};
    case DXOP_AnnotateHandle: {
        if (args.size() > 1) {
            auto properties = parseAggregateLiteral(resolveValue(ctx, args[1]));
            uint32_t property0 = 0;
            if (!properties.empty() &&
                parseUnsignedLiteral(properties[0], property0) &&
                ((property0 & 0xffu) == 3u ||
                 (property0 & 0xffu) == 8u))
                return {MSLTypeKind::RWTexture2DArray, 0, {}};
        }
        if (!args.empty()) {
            MSLType annotated = valueTypeOrUnknown(ctx, args[0]);
            if (usableType(annotated))
                return annotated;
        }
        return {MSLTypeKind::DeviceCharPtr, 0, {}};
    }
    case DXOP_TextureStore:
    case DXOP_BufferStore:
    case DXOP_RawBufferStore:
    case DXOP_Barrier:
    case DXOP_Discard:
    case 225:
    case 1026:
        return {MSLTypeKind::Void, 0, {}};
    case DXOP_CBufferLoad:
    case DXOP_CBufferLoadLegacy:
        return callee_name.find(".i32") != std::string::npos
                   ? MSLType{MSLTypeKind::UInt4, 0, {}}
                   : MSLType{MSLTypeKind::Float4, 0, {}};
    case DXOP_TextureSample:
    case DXOP_TextureSampleBias:
    case DXOP_TextureSampleLevel:
    case DXOP_TextureSampleGrad:
    case DXOP_TextureGather:
        return {MSLTypeKind::Float4, 0, {}};
    case DXOP_TextureLoad: {
        const uint32_t element_type = args.empty()
                                          ? 0u
                                          : resourceElementTypeForHandle(ctx, args[0]);
        if (element_type == 4u)
            return {MSLTypeKind::Int4, 0, {}};
        if (element_type == 5u)
            return {MSLTypeKind::UInt4, 0, {}};
        return callee_name.find(".i32") != std::string::npos
                   ? MSLType{MSLTypeKind::UInt4, 0, {}}
                   : MSLType{MSLTypeKind::Float4, 0, {}};
    }
    case DXOP_BufferLoad: {
        const uint32_t element_type = args.empty()
                                          ? 0u
                                          : resourceElementTypeForHandle(ctx, args[0]);
        if (element_type == 4u)
            return {MSLTypeKind::Int4, 0, {}};
        if (element_type == 5u)
            return {MSLTypeKind::UInt4, 0, {}};
        return callee_name.find(".i32") != std::string::npos
                   ? MSLType{MSLTypeKind::UInt4, 0, {}}
                   : MSLType{MSLTypeKind::Float4, 0, {}};
    }
    case DXOP_TextureGatherCmp:
        return {MSLTypeKind::Float4, 0, {}};
    case DXOP_TextureGatherRaw:
        return {MSLTypeKind::UInt4, 0, {} };
    case 75:
    case 76:
        return {MSLTypeKind::Float2, 0, {}};
    case 77:
        return {MSLTypeKind::UInt, 0, {}};
    case DXOP_Unpack4x8:
        return !args.empty() && literalFromValue(ctx, args[0], 0) == 1u
                   ? MSLType{MSLTypeKind::Int4, 0, {}}
                   : MSLType{MSLTypeKind::UInt4, 0, {}};
    case DXOP_Pack4x8:
        return {MSLTypeKind::UInt, 0, {}};
    case DXOP_VectorReduceAnd:
    case DXOP_VectorReduceOr:
        return {MSLTypeKind::Bool, 0, {}};
    case DXOP_FDot:
        return {MSLTypeKind::Float, 0, {}};
    case DXOP_SampleIndex:
    case DXOP_Coverage:
    case DXOP_InnerCoverage:
        return {MSLTypeKind::UInt, 0, {}};
    case DXOP_EvalSnapped:
    case DXOP_EvalSampleIndex:
    case DXOP_EvalCentroid:
    case DXOP_AttributeAtVertex:
        return usableType(declared) ? declared : MSLType{MSLTypeKind::Float, 0, {}};
    case DXOP_RawBufferLoad:
    case 303:
    case 1025:
    case DXOP_GetDimensions:
        return {MSLTypeKind::UInt4, 0, {}};
    case DXOP_TextureSampleCmp:
    case DXOP_TextureSampleCmpLevelZero:
    case DXOP_TextureSampleCmpLevel:
    case DXOP_TextureSampleCmpGrad:
    case DXOP_TextureSampleCmpBias:
    case DXOP_CalcLOD:
    case DXOP_Dot2:
    case DXOP_Dot3:
    case DXOP_Dot4:
    case DXOP_Dot2AddHalf:
        for (const auto arg : args)
            if (valueTypeOrUnknown(ctx, arg).kind == MSLTypeKind::Double)
                return {MSLTypeKind::Double, 0, {}};
        return {MSLTypeKind::Float, 0, {} };
    case DXOP_Dot4AddI8Packed:
        return {MSLTypeKind::Int, 0, {}};
    case DXOP_Dot4AddU8Packed:
        return {MSLTypeKind::UInt, 0, {}};
    case DXOP_CheckAccessFullyMapped:
    case 8:
    case 9:
    case 10:
    case 11:
    case DXOP_WaveIsFirstLane:
    case DXOP_WaveAnyTrue:
    case DXOP_WaveAllTrue:
    case DXOP_WaveActiveAllEqual:
    case DXOP_QuadVote:
    case DXOP_IsHelperLane:
        return {MSLTypeKind::Bool, 0, {}};
    case DXOP_ThreadId:
    case DXOP_GroupId:
    case DXOP_ThreadIDInGroup:
    case DXOP_FlattenedThreadIDInGroup:
    case DXOP_StartInstanceLocation:
    case DXOP_BufferUpdateCounter:
    case DXOP_AtomicBinOp:
    case DXOP_AtomicCompareExchange:
        if (callee_name.find(".i64") != std::string::npos)
            return {MSLTypeKind::Long, 0, {}};
        return {MSLTypeKind::UInt, 0, {}};
    case DXOP_StartVertexLocation:
        return {MSLTypeKind::Int, 0, {}};
    case DXOP_ViewID:
        return {MSLTypeKind::UInt, 0, {}};
    case DXOP_WaveGetLaneIndex:
    case DXOP_WaveGetLaneCount:
    case DXOP_WaveAllBitCount:
    case DXOP_WavePrefixBitCount:
        return {MSLTypeKind::UInt, 0, {}};
    case DXOP_WaveMatch:
        return {MSLTypeKind::UInt4, 0, {}};
    case DXOP_WaveMultiPrefixBitCount:
        return {MSLTypeKind::UInt, 0, {}};
    case DXOP_WaveMultiPrefixOp:
        return !args.empty() ? valueTypeOrUnknown(ctx, args[0]) : declared;
    case DXOP_LegacyF32ToF16:
        return {MSLTypeKind::UInt, 0, {}};
    case DXOP_LegacyF16ToF32:
    case DXOP_LegacyDoubleToFloat:
        return {MSLTypeKind::Float, 0, {}};
    case DXOP_LegacyDoubleToSInt32:
        return {MSLTypeKind::Int, 0, {}};
    case DXOP_LegacyDoubleToUInt32:
        return {MSLTypeKind::UInt, 0, {}};
    case DXOP_DerivCoarseX:
    case DXOP_DerivCoarseY:
    case DXOP_DerivFineX:
    case DXOP_DerivFineY:
        return args.empty() ? MSLType{MSLTypeKind::Float, 0, {}} : valueTypeOrUnknown(ctx, args[0]);
    case DXOP_MakeDouble:
        return {MSLTypeKind::Double, 0, {}};
    case DXOP_SplitDouble:
        return {MSLTypeKind::UInt2, 0, {}};
    case DXOP_BitcastI16ToF16:
        return {MSLTypeKind::Half, 0, {}};
    case DXOP_BitcastF16ToI16:
        return {MSLTypeKind::Short, 0, {}};
    case DXOP_BitcastI32ToF32:
        return {MSLTypeKind::Float, 0, {}};
    case DXOP_BitcastF32ToI32:
        return {MSLTypeKind::Int, 0, {}};
    case DXOP_BitcastI64ToF64:
        return {MSLTypeKind::Double, 0, {}};
    case DXOP_BitcastF64ToI64:
        return {MSLTypeKind::Long, 0, {}};
    case DXOP_WaveActiveBallot:
        return {MSLTypeKind::UInt4, 0, {}};
    case DXOP_WaveReadLaneAt:
    case DXOP_WaveReadLaneFirst:
    case DXOP_WaveActiveOp:
    case DXOP_WaveActiveBit:
    case DXOP_WavePrefixOp:
    case DXOP_QuadReadLaneAt:
    case DXOP_QuadOp:
        return !args.empty() ? valueTypeOrUnknown(ctx, args[0]) : declared;
    case DXOP_Unary: {
        uint32_t op = args.empty() ? 0xFFFFFFFFu : literalFromValue(ctx, args[0], 0xFFFFFFFFu);
        MSLType operand = args.size() > 1 ? valueTypeOrUnknown(ctx, args[1]) : declared;
        switch (op) {
        case DXILOP_IsNaN:
        case DXILOP_IsInf:
        case DXILOP_IsFinite:
        case DXILOP_IsNormal:
            if (DXILIRBuilder::isVectorType(operand) ||
                DXILIRBuilder::isLongVectorType(operand)) {
                MSLType result = operand;
                result.vector_element_kind = MSLTypeKind::Int;
                if (DXILIRBuilder::isVectorType(operand))
                    return DXILIRBuilder::vectorOfType(
                        {MSLTypeKind::Int, 0, {}},
                        DXILIRBuilder::vectorWidth(operand));
                return result;
            }
            return {MSLTypeKind::Bool, 0, {}};
        case DXILOP_Countbits:
        case DXILOP_FirstbitLo:
        case DXILOP_FirstbitHi:
        case DXILOP_FirstbitSHi:
            if (DXILIRBuilder::isVectorType(operand))
                return DXILIRBuilder::vectorOfType({MSLTypeKind::Int, 0, {}}, DXILIRBuilder::vectorWidth(operand));
            return {MSLTypeKind::Int, 0, {}};
        default:
            return usableType(operand) ? operand : MSLType{MSLTypeKind::Float, 0, {}};
        }
    }
    case DXOP_Binary: {
        uint32_t op = args.empty() ? 0xFFFFFFFFu : literalFromValue(ctx, args[0], 0xFFFFFFFFu);
        MSLType a = args.size() > 1 ? valueTypeOrUnknown(ctx, args[1]) : MSLType{};
        MSLType b = args.size() > 2 ? valueTypeOrUnknown(ctx, args[2]) : MSLType{};
        if (DXILIRBuilder::isLongVectorType(a) ||
            DXILIRBuilder::isLongVectorType(b)) {
            MSLType result = DXILIRBuilder::isLongVectorType(a) ? a : b;
            if (op == DXILOP_IMax || op == DXILOP_IMin || op == DXILOP_IMul)
                result.vector_element_kind = MSLTypeKind::Int;
            else if (op == DXILOP_UMax || op == DXILOP_UMin ||
                     op == DXILOP_UMul || op == DXILOP_UDiv ||
                     op == DXILOP_UAddc || op == DXILOP_USubb)
                result.vector_element_kind = MSLTypeKind::UInt;
            return result;
        }
        MSLType promoted = promoteNumericType(a, b, {MSLTypeKind::Int, 0, {}});
        if (op == DXILOP_UMax || op == DXILOP_UMin || op == DXILOP_UMul || op == DXILOP_UDiv ||
            op == DXILOP_UAddc || op == DXILOP_USubb)
            return DXILIRBuilder::isVectorType(promoted)
                ? DXILIRBuilder::vectorOfType({MSLTypeKind::UInt, 0, {}}, DXILIRBuilder::vectorWidth(promoted))
                : MSLType{MSLTypeKind::UInt, 0, {}};
        return promoted;
    }
    case DXOP_Tertiary: {
        uint32_t op = args.empty() ? 0xFFFFFFFFu : literalFromValue(ctx, args[0], 0xFFFFFFFFu);
        if (op == DXILOP_Msad)
            return {MSLTypeKind::UInt, 0, {}};
        MSLType a = args.size() > 1 ? valueTypeOrUnknown(ctx, args[1]) : MSLType{};
        MSLType b = args.size() > 2 ? valueTypeOrUnknown(ctx, args[2]) : MSLType{};
        MSLType c = args.size() > 3 ? valueTypeOrUnknown(ctx, args[3]) : MSLType{};
        if (DXILIRBuilder::isLongVectorType(a) ||
            DXILIRBuilder::isLongVectorType(b) ||
            DXILIRBuilder::isLongVectorType(c)) {
            MSLType result = DXILIRBuilder::isLongVectorType(a)
                                 ? a
                                 : DXILIRBuilder::isLongVectorType(b) ? b : c;
            if (op == DXILOP_IMad || op == DXILOP_Ibfe || op == DXILOP_Bfi)
                result.vector_element_kind = MSLTypeKind::Int;
            else if (op == DXILOP_UMad || op == DXILOP_Ubfe)
                result.vector_element_kind = MSLTypeKind::UInt;
            return result;
        }
        return promoteNumericType(promoteNumericType(a, b, declared), c, {MSLTypeKind::Int, 0, {}});
    }
    case DXOP_LoadInput:
        if (isLoadInputI32(callee_name))
            return {MSLTypeKind::Int, 0, {}};
        return usableType(declared) ? declared : MSLType{MSLTypeKind::Float, 0, {}};
    case DXOP_StoreOutput:
        return {MSLTypeKind::Void, 0, {}};
    default:
        break;
    }
    return declared;
}

static std::string resolveBindingName(const std::string &handle, const char *target_prefix) {
    if (startsWith(handle, "agg(")) {
        auto parts = parseAggregateLiteral(handle);
        uint32_t lower_bound = 0;
        if (!parts.empty()) parseUnsignedLiteral(parts[0], lower_bound);
        return std::string(target_prefix) + std::to_string(lower_bound);
    }
    for (auto *prefix : {"tex", "buf", "samp", "as"}) {
        if (startsWith(handle, prefix)) {
            return std::string(target_prefix) + std::string(handle.c_str() + std::strlen(prefix));
        }
    }
    return handle;
}

static std::string translateDXIntrinsic(LowerContext &ctx, uint32_t intrinsic_id,
                                          const std::vector<uint32_t> &args,
                                          const std::string &callee_name = {}) {
    ctx.pending_handle.reset();

    auto valueArg = [&](size_t arg, const char *fallback) -> std::string {
        if (arg >= args.size()) return fallback;
        uint32_t idx = args[arg];
        if (idx < ctx.value_table.size() && !ctx.value_table[idx].empty()) {
            const auto &v = ctx.value_table[idx];
            if (v.find('.') != std::string::npos && !exprLooksScalarLiteral(v))
                return fallback;
            return v;
        }
        for (auto &c : ctx.mod.constants)
            if (c.id == idx && !c.constant_data.empty())
                return c.constant_data;
        if (ctx.current_fn)
            for (auto &c : ctx.current_fn->constants)
                if (c.id == idx && !c.constant_data.empty())
                    return c.constant_data;
        return fallback;
    };

    auto vectorArg = [&](size_t arg, const char *fallback) -> std::string {
        if (arg >= args.size()) return fallback;
        std::string value = resolveValue(ctx, args[arg]);
        if (value.empty() || startsWith(value, "dx.") ||
            exprLooksResourceHandle(value) || exprContainsPointerSyntax(value))
            return fallback;
        return value;
    };

    auto numericArg = [&](size_t arg, const char *fallback) -> std::string {
        if (arg >= args.size()) return fallback;
        uint32_t idx = args[arg];
        std::string value = valueArg(arg, fallback);
        MSLType type = valueTypeOrUnknown(ctx, idx);
        auto pre_it = ctx.predeclared_types.find(value);
        if (pre_it != ctx.predeclared_types.end())
            type = pre_it->second;
        if (exprLooksResourceHandle(value) || typeLooksResourceHandle(type) ||
            exprContainsPointerSyntax(value) ||
            value.find("device ") != std::string::npos ||
            value.find("threadgroup ") != std::string::npos)
            return fallback;
        if (DXILIRBuilder::isVectorType(type) && !exprLooksScalarLiteral(value))
            return componentAccess(value, 0, type);
        if (exprLooksVectorValue(value))
            return "(" + value + ").x";
        return value;
    };

    auto doubleArg = [&](size_t arg, const char *fallback) -> std::string {
        std::string value = numericArg(arg, fallback);
        if (arg >= args.size()) return fallback;
        MSLType type = valueTypeOrUnknown(ctx, args[arg]);
        auto pre_it = ctx.predeclared_types.find(value);
        if (pre_it != ctx.predeclared_types.end())
            type = pre_it->second;
        if (type.kind == MSLTypeKind::Double)
            return doubleLiteralExpression(value);
        return "m12_f64_from_float(static_cast<float>(" + value + "))";
    };

    auto handleArg = [&](size_t arg, const char *prefix, const char *fallback) -> std::string {
        if (arg >= args.size()) return fallback;
        uint32_t idx = args[arg];
        auto handle_it = ctx.resource_handles.find(idx);
        if (handle_it != ctx.resource_handles.end())
            return materializeHandleName(ctx, handle_it->second, prefix);
        auto it = ctx.buffer_origin.find(idx);
        if (it != ctx.buffer_origin.end() && std::strcmp(prefix, "buf") == 0)
            return it->second;
        std::string value = valueArg(arg, fallback);
        MSLType type = valueTypeOrUnknown(ctx, idx);
        if (startsWith(value, prefix))
            return resolveBindingName(value, prefix);
        if (startsWith(value, "buf") || startsWith(value, "tex") || startsWith(value, "samp"))
            return fallback;
        if ((std::strcmp(prefix, "buf") == 0 &&
             (type.kind == MSLTypeKind::DeviceCharPtr ||
              type.kind == MSLTypeKind::ThreadgroupCharPtr)) ||
            (std::strcmp(prefix, "tex") == 0 &&
             (type.kind == MSLTypeKind::Texture2D ||
              type.kind == MSLTypeKind::RWTexture2D ||
              type.kind == MSLTypeKind::Texture2DArray ||
              type.kind == MSLTypeKind::RWTexture2DArray)) ||
            (std::strcmp(prefix, "as") == 0 &&
             type.kind == MSLTypeKind::InstanceAccelerationStructure) ||
            (std::strcmp(prefix, "samp") == 0 &&
             type.kind == MSLTypeKind::Sampler))
            return resolveBindingName(value, prefix);
        return fallback;
    };

    auto recordHandle = [&](DescriptorRangePlan::Kind kind, uint32_t resource_class,
                            uint32_t lower_bound, uint32_t binding_index,
                            uint32_t register_space = 0, bool non_uniform = false,
                            const DxilResourceBinding *metadata = nullptr,
                            const std::string &dynamic_index = {},
                            bool direct_heap = false) -> std::string {
        ResourceHandleRecord handle;
        handle.kind = kind;
        handle.resource_class = resource_class;
        handle.register_space = register_space;
        handle.lower_bound = lower_bound;
        handle.binding_count = metadata && metadata->count ? metadata->count : 1;
        handle.binding_index = handle.binding_count <= 1 ? 0 : binding_index;
        handle.non_uniform = non_uniform;
        handle.dynamic_index = dynamic_index;
        handle.direct_heap = direct_heap;
        applyResourceBindingMetadata(metadata, handle);
        ctx.pending_handle = handle;
        return materializeHandleName(ctx, handle);
    };

    auto literalArg = [&](size_t arg, uint32_t fallback, const char *label) -> uint32_t {
        if (arg >= args.size()) return fallback;
        uint32_t idx = args[arg];
        std::string text;
        for (auto &c : ctx.mod.constants)
            if (c.id == idx && !c.constant_data.empty()) { text = c.constant_data; break; }
        if (text.empty() && ctx.current_fn)
            for (auto &c : ctx.current_fn->constants)
                if (c.id == idx && !c.constant_data.empty()) { text = c.constant_data; break; }
        if (text.empty() && idx < ctx.value_table.size() && !ctx.value_table[idx].empty())
            text = ctx.value_table[idx];
        uint32_t value = 0;
        if (parseUnsignedLiteral(text, value)) return value;
        return fallback;
    };
    auto isUndefArg = [&](size_t arg) {
        if (arg >= args.size() || args[arg] == UINT32_MAX)
            return true;
        const uint32_t idx = args[arg];
        for (const auto &constant : ctx.mod.constants)
            if (constant.id == idx && constant.kind == LLVMValue::Undef)
                return true;
        if (ctx.current_fn)
            for (const auto &constant : ctx.current_fn->constants)
                if (constant.id == idx && constant.kind == LLVMValue::Undef)
                return true;
        return false;
    };
    auto isZeroLiteralArg = [&](size_t arg) {
        if (isUndefArg(arg))
            return true;
        const std::string text = valueArg(arg, "");
        uint32_t value = 0;
        return parseUnsignedLiteral(text, value) && value == 0;
    };
    auto rayQueryStateComponent = [&](const std::string &value,
                                      uint32_t component_count,
                                      const char *label) {
        const uint32_t component =
            literalArg(1, UINT32_MAX, label);
        if (component >= component_count) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL RayQuery %s component is unsupported: %u",
                             label, component);
            return std::string("0.0f");
        }
        static const char *suffixes[] = {".x", ".y", ".z", ".w"};
        return value + suffixes[component];
    };
    auto rayQueryStateMatrixComponent = [&](const char *method,
                                            const char *label) {
        const uint32_t row = literalArg(1, UINT32_MAX, label);
        const uint32_t column = literalArg(2, UINT32_MAX, label);
        if (row >= 3 || column >= 4) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx,
                             "DXIL RayQuery %s component is unsupported: %u,%u",
                             label, row, column);
            return std::string("0.0f");
        }
        // DXIL exposes a row-major 3x4 value.  Metal's float4x3 is indexed
        // by column first, so the two indices are intentionally reversed.
        return valueArg(0, "v0") + "." + method + "()[" +
               std::to_string(column) + "][" + std::to_string(row) + "]";
    };

    switch (intrinsic_id) {
    case DXOP_CreateHandle: {
        if (args.size() < 4) return "0";
        uint32_t resource_class = literalArg(0, 0, "resource class");
        uint32_t range_id = literalArg(1, 0, "range id");
        uint32_t index = literalArg(2, 0, "resource index");
        uint32_t non_uniform_raw = args.size() >= 4 ? literalArg(3, 0, "non uniform") : 0;
        if (non_uniform_raw > 1) {
            range_id = 0;
            index = 0;
            non_uniform_raw = 0;
        }
        bool non_uniform = non_uniform_raw != 0;
        ctx.next_binding++;
        const auto *metadata =
            findResourceBinding(ctx.mod, resource_class, range_id);
        std::string dynamic_index;
        if (args.size() > 2 && metadata && metadata->count > 1) {
            const std::string text = valueArg(2, "0");
            uint32_t literal = 0;
            if (!parseUnsignedLiteral(text, literal))
                dynamic_index = text;
        }
        return recordHandle(descriptorKindForResourceClass(resource_class),
                            resource_class, metadata ? metadata->lower_bound : 0,
                            index, metadata ? metadata->register_space : 0,
                            non_uniform, metadata, dynamic_index);
    }
    case DXOP_CreateHandleForLib: case DXOP_AnnotateHandle: {
        if (!args.empty()) {
            auto handle_it = ctx.resource_handles.find(args[0]);
            if (handle_it != ctx.resource_handles.end()) {
                auto annotated = handle_it->second;
                if (intrinsic_id == DXOP_AnnotateHandle && args.size() > 1) {
                    auto properties = parseAggregateLiteral(valueArg(1, ""));
                    uint32_t property0 = 0;
                    if (!properties.empty() &&
                        parseUnsignedLiteral(properties[0], property0)) {
                        annotated.resource_kind = property0 & 0xffu;
                        if (property0 & 0x1000u) {
                            annotated.kind = DescriptorRangePlan::Kind::UAV;
                            annotated.resource_class = 1;
                        } else if (annotated.kind !=
                                   DescriptorRangePlan::Kind::Sampler) {
                            annotated.kind = DescriptorRangePlan::Kind::SRV;
                            annotated.resource_class = 0;
                        }
                    }
                    if (properties.size() > 1) {
                        uint32_t property1 = 0;
                        if (parseUnsignedLiteral(properties[1], property1)) {
                            annotated.element_stride = property1;
                            if (annotated.resource_kind >= 1u &&
                                annotated.resource_kind <= 10u) {
                                annotated.element_type = property1 & 0xffu;
                                const uint32_t samples =
                                    (property1 >> 16) & 0xffu;
                                annotated.sample_count =
                                    samples ? samples : 1u;
                            }
                        }
                    }
                }
                ctx.pending_handle = annotated;
                return materializeHandleName(ctx, annotated);
            }
        }
        auto handle = valueArg(0, "tex0");
        if (startsWith(handle, "agg(")) handle = resolveBindingName(handle, "buf");
        return handle;
    }
    case DXOP_CreateHandleFromBinding: {
        auto binding = valueArg(0, "");
        auto bvals = parseAggregateLiteral(binding);
        uint32_t lower_bound = 0, upper_bound = 0, resource_class = 0;
        uint32_t count = 1, register_space = 0;
        if (bvals.size() > 0) parseUnsignedLiteral(bvals[0], lower_bound);
        if (bvals.size() > 1) parseUnsignedLiteral(bvals[1], upper_bound);
        if (bvals.size() > 2) parseUnsignedLiteral(bvals[2], register_space);
        if (bvals.size() > 3) parseUnsignedLiteral(bvals[3], resource_class);
        if (upper_bound >= lower_bound)
            count = upper_bound - lower_bound + 1;
        uint32_t index = args.size() >= 2 ? literalArg(1, 0, "idx") : 0;
        if (count != 0)
            index = std::min<uint32_t>(index, count - 1);
        bool non_uniform = args.size() >= 3 && literalArg(2, 0, "non uniform") != 0;
        const auto *metadata = findResourceBindingAtBinding(
            ctx.mod, resource_class, register_space, lower_bound);
        std::string dynamic_index;
        const uint32_t range_count = metadata && metadata->count
                                          ? metadata->count
                                          : count;
        if (args.size() >= 2 && range_count > 1) {
            const std::string text = valueArg(1, "0");
            uint32_t literal = 0;
            if (!parseUnsignedLiteral(text, literal))
                dynamic_index = text;
        }
        return recordHandle(descriptorKindForResourceClass(resource_class),
                            resource_class,
                            metadata ? metadata->lower_bound : lower_bound,
                            index, metadata ? metadata->register_space : register_space,
                            non_uniform, metadata, dynamic_index);
    }
    case DXOP_CreateHandleFromHeap: {
        uint32_t heap_index = literalArg(0, 0, "heap");
        bool sampler = args.size() >= 2 && literalArg(1, 0, "samp") != 0;
        std::string dynamic_index;
        if (!args.empty()) {
            const std::string text = valueArg(0, "0");
            uint32_t literal = 0;
            if (!parseUnsignedLiteral(text, literal)) {
                dynamic_index = text;
                heap_index = 0;
            }
        }
        std::string handle = recordHandle(
            sampler ? DescriptorRangePlan::Kind::Sampler
                    : DescriptorRangePlan::Kind::SRV,
            sampler ? 3 : 0, heap_index, 0, 0, false, nullptr,
            dynamic_index, true);
        if (!dynamic_index.empty()) {
            // Directly indexed heaps use bounded direct resource slots.
            // Preserve the dynamic index until the consuming operation can
            // select a complete buffer, texture, or sampler expression.
            ctx.pending_handle->binding_count =
                sampler ? std::min<uint32_t>(
                              4u, ctx.binding_plan.direct_sampler_count)
                        : std::min<uint32_t>(
                              8u, ctx.binding_plan.direct_buffer_count);
            handle = materializeHandleName(ctx, *ctx.pending_handle);
        }
        return handle;
    }
    case DXOP_AllocateRayQuery:
    case DXOP_AllocateRayQuery2:
        if (ctx.current_result_id == UINT32_MAX) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL ray query allocation has no result slot");
            return "";
        }
        return emitValue(ctx.current_result_id);
    case DXOP_RayQueryTraceRayInline: {
        // fn_args: query, acceleration structure, flags, mask, origin.xyz,
        // direction.xyz, Tmin, Tmax.  The first native path intentionally
        // supports the common RAY_FLAG_NONE form; other flags need explicit
        // intersection_params/tag lowering rather than being ignored.
        if (args.size() < 12) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL TraceRayInline has insufficient operands");
            return "";
        }
        const uint32_t flags = literalArg(2, UINT32_MAX, "ray flags");
        if (flags != 0u) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL TraceRayInline ray flags unsupported: %u",
                             flags);
            return "";
        }
        const std::string query = valueArg(0, "v0");
        const std::string acceleration = handleArg(1, "as", "as16");
        const uint32_t mask = literalArg(3, UINT32_MAX, "ray mask");
        if (mask == UINT32_MAX) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL TraceRayInline requires a literal mask");
            return "";
        }
        return query + ".reset(ray(float3(" + numericArg(4, "0.0f") + ", " +
               numericArg(5, "0.0f") + ", " + numericArg(6, "0.0f") +
               "), float3(" + numericArg(8, "0.0f") + ", " +
               numericArg(9, "0.0f") + ", " + numericArg(10, "0.0f") +
               "), " + numericArg(7, "0.0f") + ", " +
               numericArg(11, "0.0f") + "), " + acceleration + ", " +
               std::to_string(mask) + "u)";
    }
    case DXOP_RayQueryProceed:
        if (args.empty()) return "false";
        return valueArg(0, "v0") + ".next()";
    case DXOP_RayQueryAbort:
        if (args.empty()) return "";
        return valueArg(0, "v0") + ".abort()";
    case DXOP_RayQueryCommitNonOpaqueTriangleHit:
        if (args.empty()) return "";
        return valueArg(0, "v0") + ".commit_triangle_intersection()";
    case DXOP_RayQueryCommitProceduralPrimitiveHit:
        if (args.empty()) return "";
        return valueArg(0, "v0") + ".commit_bounding_box_intersection(" +
               numericArg(1, "0.0f") + ")";
    case DXOP_RayQueryCandidateType:
        if (args.empty()) return "0u";
        return "(" + valueArg(0, "v0") +
               ".get_candidate_intersection_type() == intersection_type::triangle ? 0u : 1u)";
    case DXOP_RayQueryCommittedStatus:
        if (args.empty()) return "0u";
        return "(" + valueArg(0, "v0") +
               ".get_committed_intersection_type() == intersection_type::triangle ? 1u : "
               "(" + valueArg(0, "v0") +
               ".get_committed_intersection_type() == intersection_type::bounding_box ? 2u : 0u))";
    case 190:
        if (args.empty()) return "0u";
        return "(" + valueArg(0, "v0") +
               ".is_candidate_non_opaque_bounding_box() ? 1u : 0u)";
    case 191:
        if (args.empty()) return "0u";
        return "(" + valueArg(0, "v0") +
               ".is_candidate_triangle_front_facing() ? 1u : 0u)";
    case 192:
        if (args.empty()) return "0u";
        return "(" + valueArg(0, "v0") +
               ".is_committed_triangle_front_facing() ? 1u : 0u)";
    case 195:
        // TraceRayInline currently accepts only RAY_FLAG_NONE, so the value
        // is exactly the only supported flag set.
        return "0u";
    case 198:
        if (args.empty()) return "0.0f";
        return valueArg(0, "v0") + ".get_ray_min_distance()";
    case 199:
        if (args.empty()) return "0.0f";
        return valueArg(0, "v0") + ".get_candidate_triangle_distance()";
    case 200:
        if (args.empty()) return "0.0f";
        return valueArg(0, "v0") + ".get_committed_distance()";
    case 201:
        if (args.empty()) return "0u";
        return valueArg(0, "v0") + ".get_candidate_instance_id()";
    case 202:
        if (args.empty()) return "0u";
        return valueArg(0, "v0") + ".get_candidate_user_instance_id()";
    case 203:
        if (args.empty()) return "0u";
        return valueArg(0, "v0") + ".get_candidate_geometry_id()";
    case 204:
        if (args.empty()) return "0u";
        return valueArg(0, "v0") + ".get_candidate_primitive_id()";
    case 207:
        if (args.empty()) return "0u";
        return valueArg(0, "v0") + ".get_committed_instance_id()";
    case 208:
        if (args.empty()) return "0u";
        return valueArg(0, "v0") + ".get_committed_user_instance_id()";
    case 209:
        if (args.empty()) return "0u";
        return valueArg(0, "v0") + ".get_committed_geometry_id()";
    case 210:
        if (args.empty()) return "0u";
        return valueArg(0, "v0") + ".get_committed_primitive_id()";
    case 193:
        if (args.size() < 2) return "0.0f";
        return rayQueryStateComponent(
            valueArg(0, "v0") + ".get_candidate_triangle_barycentric_coord()",
            2, "candidate barycentrics");
    case 194:
        if (args.size() < 2) return "0.0f";
        return rayQueryStateComponent(
            valueArg(0, "v0") + ".get_committed_triangle_barycentric_coord()",
            2, "committed barycentrics");
    case 196:
        if (args.size() < 2) return "0.0f";
        return rayQueryStateComponent(
            valueArg(0, "v0") + ".get_world_space_ray_origin()", 3,
            "world ray origin");
    case 197:
        if (args.size() < 2) return "0.0f";
        return rayQueryStateComponent(
            valueArg(0, "v0") + ".get_world_space_ray_direction()", 3,
            "world ray direction");
    case 205:
        if (args.size() < 2) return "0.0f";
        return rayQueryStateComponent(
            valueArg(0, "v0") + ".get_candidate_ray_origin()", 3,
            "candidate object ray origin");
    case 206:
        if (args.size() < 2) return "0.0f";
        return rayQueryStateComponent(
            valueArg(0, "v0") + ".get_candidate_ray_direction()", 3,
            "candidate object ray direction");
    case 211:
        if (args.size() < 2) return "0.0f";
        return rayQueryStateComponent(
            valueArg(0, "v0") + ".get_committed_ray_origin()", 3,
            "committed object ray origin");
    case 212:
        if (args.size() < 2) return "0.0f";
        return rayQueryStateComponent(
            valueArg(0, "v0") + ".get_committed_ray_direction()", 3,
            "committed object ray direction");
    case 186:
        return rayQueryStateMatrixComponent(
            "get_candidate_object_to_world_transform",
            "candidate object-to-world");
    case 187:
        return rayQueryStateMatrixComponent(
            "get_candidate_world_to_object_transform",
            "candidate world-to-object");
    case 188:
        return rayQueryStateMatrixComponent(
            "get_committed_object_to_world_transform",
            "committed object-to-world");
    case 189:
        return rayQueryStateMatrixComponent(
            "get_committed_world_to_object_transform",
            "committed world-to-object");
    case 214:
        if (args.empty()) return "0u";
        return "(buf30 == nullptr ? 0u : reinterpret_cast<device uint*>(buf30)[" +
               valueArg(0, "v0") + ".get_candidate_instance_id()])";
    case 215:
        if (args.empty()) return "0u";
        return "(buf30 == nullptr ? 0u : reinterpret_cast<device uint*>(buf30)[" +
               valueArg(0, "v0") + ".get_committed_instance_id()])";
    case DXOP_StartVertexLocation:
        if (ctx.shader.kind != DxilShaderKind::Vertex) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx,
                             "DXIL StartVertexLocation is only valid in a vertex shader");
            return "0";
        }
        return "(buf29 == nullptr ? 0 : (int)(m12_is_indexed_draw(buf30) ? "
               "reinterpret_cast<device m12_draw_indexed_argument*>(buf29)->baseVertexLocation : "
               "reinterpret_cast<device m12_draw_argument*>(buf29)->startVertexLocation))";
    case DXOP_StartInstanceLocation:
        if (ctx.shader.kind != DxilShaderKind::Vertex) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx,
                             "DXIL StartInstanceLocation is only valid in a vertex shader");
            return "0u";
        }
        return "(buf29 == nullptr ? 0u : (m12_is_indexed_draw(buf30) ? "
               "reinterpret_cast<device m12_draw_indexed_argument*>(buf29)->startInstanceLocation : "
               "reinterpret_cast<device m12_draw_argument*>(buf29)->startInstanceLocation))";
    case DXOP_ViewID:
        if (ctx.shader.kind != DxilShaderKind::Vertex) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx,
                             "DXIL ViewID is only lowered for vertex-stage view replay");
            return "0u";
        }
        return "(buf30 == nullptr ? 0u : uint(*reinterpret_cast<device ushort*>(buf30 + 2)))";
    case DXOP_ThreadId: {
        ctx.uses_thread_id = true;
        uint32_t c = args.empty() ? 0 : literalArg(0, 0, "comp");
        if (c == 0) return "(int)dtid.x"; if (c == 1) return "(int)dtid.y"; return "(int)dtid.z";
    }
    case DXOP_GroupId: {
        ctx.uses_group_id = true;
        uint32_t c = args.empty() ? 0 : literalArg(0, 0, "comp");
        if (c == 0) return "(int)ggid.x"; if (c == 1) return "(int)ggid.y"; return "(int)ggid.z";
    }
    case DXOP_ThreadIDInGroup: {
        ctx.uses_group_thread_id = true;
        uint32_t c = args.empty() ? 0 : literalArg(0, 0, "comp");
        if (c == 0) return "(int)gtid.x"; if (c == 1) return "(int)gtid.y"; return "(int)gtid.z";
    }
    case DXOP_FlattenedThreadIDInGroup:
        ctx.uses_group_thread_id = true; ctx.uses_group_size = true;
        return "(int)(gtid.x + gtid.y * gsz.x + gtid.z * gsz.x * gsz.y)";
    case DXOP_WriteSamplerFeedback:
    case DXOP_WriteSamplerFeedbackBias:
    case DXOP_WriteSamplerFeedbackLevel:
    case DXOP_WriteSamplerFeedbackGrad: {
        if (args.size() < 7) return "";
        ctx.uses_sampler_feedback = true;
        auto feedback = handleArg(0, "buf", "buf0");
        auto sampled = handleArg(1, "tex", "tex0");
        auto sampler = handleArg(2, "samp", "samp0");
        auto c0 = numericArg(3, "0.0");
        auto c1 = numericArg(4, "0.0");
        auto c2 = numericArg(5, "0.0");
        std::string lod = "0.0";
        if (intrinsic_id == DXOP_WriteSamplerFeedbackLevel && args.size() >= 8) {
            lod = numericArg(7, "0.0");
        } else if (intrinsic_id == DXOP_WriteSamplerFeedbackGrad &&
                   args.size() >= 13) {
            auto ddx0 = numericArg(7, "0.0");
            auto ddx1 = numericArg(8, "0.0");
            auto ddy0 = numericArg(10, "0.0");
            auto ddy1 = numericArg(11, "0.0");
            lod = "max(0.0, log2(max(length(float2(" + ddx0 + ", " + ddx1 + ") * float2(" +
                  sampled + ".get_width(), " + sampled + ".get_height())), length(float2(" +
                  ddy0 + ", " + ddy1 + ") * float2(" + sampled + ".get_width(), " +
                  sampled + ".get_height())))))";
        } else {
            lod = sampled + ".calculate_clamped_lod(" + sampler + ", float2(" + c0 + ", " + c1 + "))";
            if (intrinsic_id == DXOP_WriteSamplerFeedbackBias && args.size() >= 8)
                lod = "(" + lod + " + " + numericArg(7, "0.0") + ")";
        }
        lod = "clamp(" + lod + ", 0.0, float(max(" + sampled +
              ".get_num_mip_levels(), 1u) - 1u))";
        return "m12_write_sampler_feedback(" + feedback + ", float3(" + c0 + ", " + c1 + ", " + c2 + "), " + lod + ")";
    }
    case DXOP_CBufferLoad: case DXOP_CBufferLoadLegacy: {
        if (args.size() < 2) return "float4(0)";
        auto handle = handleArg(0, "buf", "buf0");
        auto handle_record = ctx.resource_handles.find(args[0]);
        if (handle_record == ctx.resource_handles.end() ||
            handle_record->second.kind != DescriptorRangePlan::Kind::CBV) {
            // Some DXIL 1.x modules leave the cbuffer handle's binding
            // record unresolved after resource annotation.  CBufferLoad is
            // still unambiguous: recover the first CBV range rather than
            // silently reading the SRV texture namespace.
            for (const auto &range : ctx.binding_plan.ranges) {
                if (range.kind == DescriptorRangePlan::Kind::CBV) {
                    handle = "buf" + std::to_string(range.lower_bound + 8u);
                    break;
                }
            }
        }
        if (!startsWith(handle, "buf"))
            return "float4(0)";
        ctx.last_buffer_handle = handle;
        auto reg = ensureScalarIndex(numericArg(1, "0"));
        const char *element = callee_name.find(".i32") != std::string::npos
                                  ? "uint4"
                                  : "float4";
        return "(reinterpret_cast<device " + std::string(element) + "&>(" +
               handle + "[((int)(" + reg + "))*64]))";
    }
    case DXOP_BufferLoad: {
        if (args.size() < 3) return "float4(0)";
        auto handle = handleArg(0, "buf", "buf0");
        if (handle.find("buf") == std::string::npos)
            return "float4(0)";
        ctx.last_buffer_handle = handle;
        auto idx = ensureScalarIndex(numericArg(1, "0"));
        uint32_t resource_kind = 0;
        uint32_t element_stride = 0;
        auto handle_it = ctx.resource_handles.find(args[0]);
        if (handle_it != ctx.resource_handles.end()) {
            resource_kind = handle_it->second.resource_kind;
            element_stride = handle_it->second.element_stride;
        }
        std::string byte_offset;
        if (resource_kind == 0u || resource_kind == 11u) {
            byte_offset = "((int)(" + idx + "))";
        } else if (resource_kind == 12u) {
            if (element_stride == 0)
                element_stride = 16;
            byte_offset = "((int)(" + idx + ") * " +
                          std::to_string(element_stride) + " + (int)(" +
                          ensureScalarIndex(numericArg(2, "0")) + "))";
        } else {
            if (element_stride == 0)
                element_stride = 16;
                byte_offset = "((int)(" + idx + ") * " +
                          std::to_string(element_stride) + ")";
        }
        if (callee_name.find(".i32") != std::string::npos &&
            handle.find("? buf") != std::string::npos) {
            const size_t index_start = handle.find("uint(");
            const size_t index_end = index_start == std::string::npos
                                         ? std::string::npos
                                         : handle.find(')', index_start + 5);
            const size_t first_buffer = handle.find("? buf");
            std::string dynamic_index;
            uint32_t base = 0;
            uint32_t count = 0;
            if (index_start != std::string::npos &&
                index_end != std::string::npos &&
                first_buffer != std::string::npos) {
                dynamic_index = handle.substr(index_start + 5,
                                              index_end - index_start - 5);
                size_t cursor = first_buffer;
                while (cursor != std::string::npos) {
                    cursor += 5;
                    size_t end = cursor;
                    while (end < handle.size() &&
                           std::isdigit(static_cast<unsigned char>(handle[end])))
                        ++end;
                    uint32_t candidate = 0;
                    if (count == 0 && parseUnsignedLiteral(
                            handle.substr(cursor, end - cursor), candidate))
                        base = candidate;
                    ++count;
                    cursor = handle.find("buf", end);
                }
            }
            if (!dynamic_index.empty() && count > 1) {
                const std::string helper = "m12_dynamic_buffer_load_" +
                                           std::to_string(base) + "_" +
                                           std::to_string(count) + "(uint(" +
                                           dynamic_index + "), " + byte_offset;
                std::string call = helper;
                for (uint32_t i = 0; i < count; ++i)
                    call += ", buf" + std::to_string(base + i);
                return call + ")";
            }
        }
        if (handle_it != ctx.resource_handles.end() &&
            handle_it->second.kind == DescriptorRangePlan::Kind::SRV &&
            !handle_it->second.dynamic_index.empty() &&
            handle_it->second.binding_count > 1 &&
            callee_name.find(".i32") != std::string::npos) {
            const uint32_t base = directBufferBindingIndex(
                ctx, handle_it->second, "buf");
            const uint32_t count = std::min<uint32_t>(
                handle_it->second.binding_count,
                ctx.binding_plan.direct_buffer_count > base
                    ? ctx.binding_plan.direct_buffer_count - base
                    : 0);
            if (count > 1) {
                std::string helper = "m12_dynamic_buffer_load_" +
                                     std::to_string(base) + "_" +
                                     std::to_string(count) + "(uint(" +
                                     handle_it->second.dynamic_index + "), " +
                                     byte_offset;
                for (uint32_t i = 0; i < count; ++i)
                    helper += ", buf" + std::to_string(base + i);
                return helper + ")";
            }
        }
        if (callee_name.find(".i32") != std::string::npos)
            return "uint4(reinterpret_cast<device uint&>(" + handle + "[" +
                   byte_offset + "]), 0u, 0u, 0u)";
        return "(reinterpret_cast<device float4&>(" + handle + "[" +
               byte_offset + "]))";
    }
    case DXOP_RawBufferLoad: case 303: case 1025: {
        if (args.size() < 3) return "uint4(0)";
        auto handle = handleArg(0, "buf", "buf0");
        if (handle.find("buf") == std::string::npos)
            return "uint4(0)";
        ctx.last_buffer_handle = handle;
        auto idx = ensureScalarIndex(numericArg(1, "0"));
        auto off = ensureScalarIndex(numericArg(2, "0"));
        uint32_t resource_kind = 0;
        uint32_t element_stride = 0;
        auto handle_it = ctx.resource_handles.find(args[0]);
        if (handle_it != ctx.resource_handles.end()) {
            resource_kind = handle_it->second.resource_kind;
            element_stride = handle_it->second.element_stride;
        }
        std::string raw_byte_offset;
        if (resource_kind == 12u) {
            if (element_stride == 0)
                element_stride = 4;
            raw_byte_offset = "(((int)(" + idx + ")) * " +
                              std::to_string(element_stride) +
                              " + ((int)(" + off + ")))";
        } else {
            // ByteAddressBuffer coordinates are already byte offsets.  The
            // structured-buffer form uses coord0 as an element index and
            // coord1 as the byte offset within that element.
            raw_byte_offset = "(((int)(" + idx + ")) + ((int)(" + off + ")))";
        }
        if (handle.find("? buf") != std::string::npos) {
            const size_t index_start = handle.find("uint(");
            const size_t index_end = index_start == std::string::npos
                                         ? std::string::npos
                                         : handle.find(')', index_start + 5);
            const size_t first_buffer = handle.find("? buf");
            std::string dynamic_index;
            uint32_t base = 0;
            uint32_t count = 0;
            if (index_start != std::string::npos &&
                index_end != std::string::npos &&
                first_buffer != std::string::npos) {
                dynamic_index = handle.substr(index_start + 5,
                                              index_end - index_start - 5);
                size_t cursor = first_buffer;
                while (cursor != std::string::npos) {
                    cursor += 5;
                    size_t end = cursor;
                    while (end < handle.size() &&
                           std::isdigit(static_cast<unsigned char>(handle[end])))
                        ++end;
                    uint32_t candidate = 0;
                    if (count == 0 && parseUnsignedLiteral(
                            handle.substr(cursor, end - cursor), candidate))
                        base = candidate;
                    ++count;
                    cursor = handle.find("buf", end);
                }
            }
            if (!dynamic_index.empty() && count > 1) {
                std::string call = "m12_dynamic_buffer_load_" +
                                   std::to_string(base) + "_" +
                                   std::to_string(count) + "(uint(" +
                                   dynamic_index + "), " + raw_byte_offset;
                for (uint32_t i = 0; i < count; ++i)
                    call += ", buf" + std::to_string(base + i);
                return call + ")";
            }
        }
        return "(reinterpret_cast<device uint4&>(" + handle + "[" +
               raw_byte_offset + "]))";
    }
    case DXOP_BufferStore: case DXOP_RawBufferStore: case 1026: {
        if (args.size() < 4) return "";
        auto handle = handleArg(0, "buf", "buf0");
        if (handle.find("buf") == std::string::npos)
            return "";
        auto idx = ensureScalarIndex(numericArg(1, "0"));
        auto off = ensureScalarIndex(numericArg(2, "0"));
        // BufferStore coordinates are byte offsets for raw/byte-address
        // resources. Respect the DXIL write mask so undefined components do
        // not overwrite adjacent lanes.
        uint32_t resource_kind = 0;
        uint32_t element_stride = 0;
        auto handle_it = ctx.resource_handles.find(args[0]);
        if (handle_it != ctx.resource_handles.end()) {
            resource_kind = handle_it->second.resource_kind;
            element_stride = handle_it->second.element_stride;
        }
        std::string base;
        if (resource_kind == 12u) {
            if (element_stride == 0)
                element_stride = 16;
            base = "(((int)(" + idx + ")) * " +
                   std::to_string(element_stride) + " + ((int)(" + off + ")))";
        } else if (resource_kind == 10u) {
            if (element_stride == 0)
                element_stride = 16;
            base = "(((int)(" + idx + ")) * " +
                   std::to_string(element_stride) + ")";
        } else {
            base = "(((int)(" + idx + ")) + ((int)(" + off + ")))";
        }
        std::ostringstream store;
        uint32_t value_count =
            std::min<uint32_t>(4, static_cast<uint32_t>(args.size()) - 4);
        size_t mask_index =
            intrinsic_id == DXOP_RawBufferStore || intrinsic_id == 1026
                ? args.size() - 2
                : args.size() - 1;
        uint32_t mask = literalArg(mask_index, 0xf, "buffer_store_mask");
        bool emitted = false;
        const bool i64_store =
            callee_name.find(".i64") != std::string::npos;
        for (uint32_t i = 0; i < value_count; i++) {
            if (!(mask & (1u << i)))
                continue;
            if (emitted) store << ";\n  ";
            if (i64_store) {
                store << "reinterpret_cast<device ulong&>(" << handle
                      << "[(" << base << ") + " << (i * 8)
                      << "]) = (ulong)(" << numericArg(3 + i, "0") << ")";
            } else {
                store << "reinterpret_cast<device uint&>(" << handle
                      << "[(" << base << ") + " << (i * 4)
                      << "]) = (uint)(" << numericArg(3 + i, "0") << ")";
            }
            emitted = true;
        }
        return store.str();
    }
    case 304: {
        if (args.size() < 4) return "";
        auto handle = handleArg(0, "buf", "buf0");
        auto idx = ensureScalarIndex(numericArg(1, "0"));
        auto off = ensureScalarIndex(numericArg(2, "0"));
        auto val = valueArg(3, "0");
        MSLType value_type = valueTypeOrUnknown(ctx, args[3]);
        // DXC 1.9 represents the vector-store operand as an ExtractValue of
        // the preceding RawBufferVectorLoad result. Recover that aggregate
        // instead of repeating its x component across all four stores.
        auto origin_it = ctx.vector_extract_origin.find(args[3]);
        if (origin_it != ctx.vector_extract_origin.end() &&
            origin_it->second < ctx.value_types.size() &&
            DXILIRBuilder::isVectorType(ctx.value_types[origin_it->second])) {
            val = emitValue(origin_it->second);
            value_type = ctx.value_types[origin_it->second];
        }
        if (!DXILIRBuilder::isVectorType(value_type) &&
            val.rfind("(v", 0) == 0) {
            const size_t close = val.find(")", 2);
            if (close != std::string::npos && val.substr(close) == ").x") {
                uint32_t aggregate_id = 0;
                if (parseEmittedValueName(val.substr(1, close - 1),
                                          aggregate_id) &&
                    aggregate_id < ctx.value_types.size() &&
                    DXILIRBuilder::isVectorType(ctx.value_types[aggregate_id])) {
                    val = emitValue(aggregate_id);
                    value_type = ctx.value_types[aggregate_id];
                }
            }
        }
        uint32_t resource_kind = 0;
        uint32_t element_stride = 0;
        auto handle_it = ctx.resource_handles.find(args[0]);
        if (handle_it != ctx.resource_handles.end()) {
            resource_kind = handle_it->second.resource_kind;
            element_stride = handle_it->second.element_stride;
        }
        std::string base;
        if (resource_kind == 12u || resource_kind == 10u) {
            if (element_stride == 0)
                element_stride = 16;
            base = "(((int)(" + idx + ")) * " +
                   std::to_string(element_stride) + " + ((int)(" + off + ")))";
        } else {
            base = "(((int)(" + idx + ")) + ((int)(" + off + ")))";
        }
        std::ostringstream store;
        for (uint32_t i = 0; i < 4; i++) {
            if (i) store << ";\n  ";
            std::string component =
                DXILIRBuilder::isVectorType(value_type)
                    ? componentAccess(val, i, value_type)
                    : (i == 0 ? val : "0");
            store << "reinterpret_cast<device uint&>(" << handle << "[(" << base << ") + " << (i*4)
                  << "]) = (uint)(" << component << ")";
        }
        return store.str();
    }
    case DXOP_TextureLoad: {
        if (args.size() < 3) return "float4(0)";
        auto handle = handleArg(0, "tex", "tex0");
        ctx.last_buffer_handle = handle;
        const uint32_t resource_kind = resourceKindForHandle(ctx, args[0]);
        auto mip = ensureScalarIndex(numericArg(1, "0"));
        auto c0 = textureCoordComponent(ctx, valueArg(2, "0"), 0);
        auto c1 = textureCoordComponent(ctx, valueArg(3, "0"), 1);
        auto c2 = textureCoordComponent(ctx, valueArg(4, "0"), 2);
        // Writable MSAA resources retain the existing flattened array ABI;
        // their DXIL sample operand occupies the mip slot.
        if (isWritableMSAAHandle(ctx, args[0])) {
            auto sample = ensureScalarIndex(numericArg(1, "0"));
            auto array_slice = isWritableMSAAArrayHandle(ctx, args[0])
                                   ? ensureScalarIndex(numericArg(4, "0"))
                                   : "0";
            return handle + ".read(uint2(" + c0 + ", " + c1 +
                   "), " + writableMSAASlice(ctx, args[0], sample,
                                               array_slice) + ")";
        }
        if (ctx.compute_sample_cmp_shader) {
            return "float4(" + handle + ".read(uint2(" + c0 + ", " + c1 +
                   "), (uint)(" + mip + ")))";
        }
        auto texture_read = [&](const std::string &texture) {
            switch (resource_kind) {
            case 1u:
                return texture + ".read(uint2((uint)(" + c0 + "), 0u), (uint)(" +
                       mip + "))";
            case 6u:
                return texture + ".read(uint2((uint)(" + c0 + "), 0u), (uint)(" +
                       c1 + "), (uint)(" + mip + "))";
            case 2u:
                return texture + ".read(uint2(" + c0 + ", " + c1 +
                       "), (uint)(" + mip + "))";
            case 7u:
                return texture + ".read(uint2(" + c0 + ", " + c1 +
                       "), (uint)(" + c2 + "), (uint)(" + mip + "))";
            case 4u:
                return texture + ".read(uint3(" + c0 + ", " + c1 + ", " + c2 +
                       "), (uint)(" + mip + "))";
            case 3u:
                return texture + ".read(uint2(" + c0 + ", " + c1 +
                       "), (uint)(" + mip + "))";
            case 8u:
                return texture + ".read(uint2(" + c0 + ", " + c1 +
                       "), (uint)(" + c2 + "), (uint)(" + mip + "))";
            default:
                return texture + ".read(uint2(" + c0 + ", " + c1 + "))";
            }
        };
        // A directly indexed texture heap cannot materialize a Metal texture
        // object through a pointer ternary. Select between complete read
        // expressions instead, bounded by the direct texture ABI.
        auto dynamic_handle = ctx.resource_handles.find(args[0]);
        if (dynamic_handle != ctx.resource_handles.end() &&
            !dynamic_handle->second.dynamic_index.empty() &&
            dynamic_handle->second.binding_count > 1) {
            const uint32_t base = dynamic_handle->second.lower_bound;
            const uint32_t count = std::min<uint32_t>(
                dynamic_handle->second.binding_count,
                ctx.binding_plan.direct_texture_count > base
                    ? ctx.binding_plan.direct_texture_count - base
                    : 0);
            if (count > 1) {
                std::string selected = texture_read(
                    "tex" + std::to_string(base + count - 1));
                for (uint32_t i = count - 1; i > 0; --i) {
                    selected = "((uint(" +
                               dynamic_handle->second.dynamic_index + ") == " +
                               std::to_string(base + i - 1) + "u) ? " +
                               texture_read("tex" +
                                            std::to_string(base + i - 1)) +
                               " : " + selected + ")";
                }
                const uint32_t element_type =
                    resourceElementTypeForHandle(ctx, args[0]);
                const char *result_type =
                    isIntegerResourceElementType(element_type)
                        ? (isSignedResourceElementType(element_type) ? "int4"
                                                                    : "uint4")
                        : "float4";
                return std::string(result_type) + "(" + selected + ")";
            }
        }
        // Cube Load is not a valid HLSL operation in the shader models
        // handled here. Keep the old 2D fallback only for modules whose
        // resource metadata did not identify a dimension.
        if (resource_kind == 5u || resource_kind == 9u) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx,
                             "DXIL textureLoad is unsupported for cube resource kind=%u",
                             resource_kind);
            return "float4(0)";
        }
        return texture_read(handle);
    }
    case DXOP_TextureStore: case 225: {
        if (args.size() < 6) return "";
        auto handle = handleArg(0, "tex", "tex0");
        const uint32_t resource_kind = resourceKindForHandle(ctx, args[0]);
        auto c0 = ensureScalarIndex(numericArg(1, "0"));
        auto c1 = ensureScalarIndex(numericArg(2, "0"));
        auto c2 = ensureScalarIndex(numericArg(3, "0"));
        size_t vb = 4;
        const uint32_t element_type = resourceElementTypeForHandle(ctx, args[0]);
        const bool integer_element = isIntegerResourceElementType(element_type);
        const char *value_type = integer_element
                                      ? (isSignedResourceElementType(element_type) ? "int4" : "uint4")
                                      : "float4";
        const char *literal_fallback = integer_element ? "0" : "0.0";
        std::string value = std::string(value_type) + "(" + numericArg(vb, literal_fallback) + ", " +
                            numericArg(vb+1, literal_fallback) + ", " +
                            numericArg(vb+2, literal_fallback) + ", " +
                            numericArg(vb+3, literal_fallback) + ")";
        if (startsWith(handle, "buf"))
            return "reinterpret_cast<device float4&>(" + handle + "[(((int)(" + c1 + "))*4096 + ((int)(" + c0 + "))*16)]) = " + value;
        if (intrinsic_id == DXOP_TextureStoreSample &&
            isWritableMSAAHandle(ctx, args[0])) {
            auto sample = ensureScalarIndex(numericArg(9, "0"));
            auto array_slice = isWritableMSAAArrayHandle(ctx, args[0])
                                   ? c2
                                   : "0";
            return handle + ".write(" + value + ", uint2((uint)(" + c0 + "), (uint)(" + c1 + ")), " +
                   writableMSAASlice(ctx, args[0], sample, array_slice) + ")";
        }
        auto texture_write = [&](const std::string &texture) {
            switch (resource_kind) {
            case 1u:
                return texture + ".write(" + value + ", uint2((uint)(" + c0 + "), 0u))";
            case 6u:
                return texture + ".write(" + value + ", uint2((uint)(" + c0 + "), 0u), (uint)(" + c1 + "))";
            case 2u:
                return texture + ".write(" + value + ", uint2((uint)(" + c0 + "), (uint)(" + c1 + ")))";
            case 7u:
                return texture + ".write(" + value + ", uint2((uint)(" + c0 + "), (uint)(" + c1 + ")), (uint)(" + c2 + "))";
            case 4u:
                return texture + ".write(" + value + ", uint3((uint)(" + c0 + "), (uint)(" + c1 + "), (uint)(" + c2 + ")))";
            default:
                return texture + ".write(" + value + ", uint2((uint)(" + c0 + "), (uint)(" + c1 + ")))";
            }
        };
        auto dynamic_handle = ctx.resource_handles.find(args[0]);
        if (dynamic_handle != ctx.resource_handles.end() &&
            !dynamic_handle->second.dynamic_index.empty() &&
            dynamic_handle->second.binding_count > 1) {
            const uint32_t base = dynamic_handle->second.lower_bound;
            const uint32_t count = std::min<uint32_t>(
                dynamic_handle->second.binding_count,
                ctx.binding_plan.direct_texture_count > base
                    ? ctx.binding_plan.direct_texture_count - base
                    : 0);
            if (count > 1) {
                std::ostringstream selected;
                for (uint32_t i = 0; i < count; ++i) {
                    if (i == 0)
                        selected << "if (uint("
                                 << dynamic_handle->second.dynamic_index
                                 << ") == " << (base + i) << "u) ";
                    else if (i + 1 < count)
                        selected << ";\n  else if (uint("
                                 << dynamic_handle->second.dynamic_index
                                 << ") == " << (base + i) << "u) ";
                    else
                        selected << ";\n  else ";
                    selected << texture_write(
                        "tex" + std::to_string(base + i));
                }
                return selected.str();
            }
        }
        if (resource_kind == 5u || resource_kind == 9u) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx,
                             "DXIL textureStore is unsupported for cube resource kind=%u",
                             resource_kind);
            return "";
        }
        return texture_write(handle);
    }
    case DXOP_TextureSample: case DXOP_TextureSampleBias:
    case DXOP_TextureSampleLevel: case DXOP_TextureSampleGrad: {
        if (args.size() < 4) return "float4(0)";
        auto handle = handleArg(0, "tex", "tex0");
        auto samp = handleArg(1, "samp", "samp0");
        const uint32_t resource_kind = resourceKindForHandle(ctx, args[0]);
        auto c0 = sampleCoordComponent(ctx, valueArg(2, "0.0"), 0);
        auto c1 = sampleCoordComponent(ctx, valueArg(3, "0.0"), 1);
        auto c2 = sampleCoordComponent(ctx, valueArg(4, "0.0"), 2);
        auto c3 = sampleCoordComponent(ctx, valueArg(5, "0.0"), 3);
        auto ox = ensureScalarIndex(numericArg(6, "0"));
        auto oy = ensureScalarIndex(numericArg(7, "0"));
        auto oz = ensureScalarIndex(numericArg(8, "0"));

        std::string coord;
        std::string array_index;
        bool has_array_index = false;
        bool has_offset = true;
        bool is_3d_gradient = false;
        switch (resource_kind) {
        case 1u:
            coord = "float2(" + c0 + ", 0.5f)";
            break;
        case 6u:
            coord = "float2(" + c0 + ", 0.5f)";
            array_index = "(uint)(" + c1 + ")";
            has_array_index = true;
            break;
        case 2u:
            coord = "float2(" + c0 + ", " + c1 + ")";
            break;
        case 7u:
            coord = "float2(" + c0 + ", " + c1 + ")";
            array_index = "(uint)(" + c2 + ")";
            has_array_index = true;
            break;
        case 4u:
            coord = "float3(" + c0 + ", " + c1 + ", " + c2 + ")";
            is_3d_gradient = true;
            break;
        case 5u:
            coord = "float3(" + c0 + ", " + c1 + ", " + c2 + ")";
            has_offset = false;
            is_3d_gradient = true;
            break;
        case 9u:
            coord = "float3(" + c0 + ", " + c1 + ", " + c2 + ")";
            array_index = "(uint)(" + c3 + ")";
            has_array_index = true;
            has_offset = false;
            is_3d_gradient = true;
            break;
        case 3u:
        case 8u:
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL texture sample is unsupported for multisample resource kind=%u", resource_kind);
            return "float4(0)";
        default:
            coord = "float2(" + c0 + ", " + c1 + ")";
            break;
        }
        // The DXIL call has fixed-width offset operands.  An omitted HLSL
        // offset is encoded as undef; do not turn that placeholder into a
        // real one-texel Metal offset.
        if (has_offset &&
            (isUndefArg(6) ||
             (isZeroLiteralArg(6) && isZeroLiteralArg(7) &&
              isZeroLiteralArg(8))))
            has_offset = false;
        if (has_offset) {
            // Do not use Metal's integer sample-offset overload here.  Its
            // address quantization differs from HLSL's texel-relative offset
            // for normalized coordinates.  Apply the offset in normalized
            // texture space instead, which also handles a dynamic offset.
            auto shifted = [&](const std::string &value,
                               const std::string &offset,
                               const char *dimension) {
                return "((float)(" + value + ") + (float)(" + offset +
                       ") / float(" + handle + ".get_" + dimension + "()))";
            };
            if (resource_kind == 4u) {
                coord = "float3(" + shifted(c0, ox, "width") + ", " +
                        shifted(c1, oy, "height") + ", " +
                        shifted(c2, oz, "depth") + ")";
            } else if (resource_kind == 1u || resource_kind == 6u) {
                coord = "float2(" + shifted(c0, ox, "width") + ", 0.5f)";
            } else if (resource_kind == 2u || resource_kind == 7u) {
                coord = "float2(" + shifted(c0, ox, "width") + ", " +
                        shifted(c1, oy, "height") + ")";
            }
            has_offset = false;
        }

        std::string call = handle + ".sample(" + samp + ", " + coord;
        if (has_array_index)
            call += ", " + array_index;
        if (intrinsic_id == DXOP_TextureSampleLevel) {
            call += ", level((float)(" + numericArg(9, "0.0") + "))";
        } else if (intrinsic_id == DXOP_TextureSampleBias) {
            call += ", bias((float)(" + numericArg(9, "0.0") + "))";
        } else if (intrinsic_id == DXOP_TextureSampleGrad) {
            if (resource_kind == 1u || resource_kind == 6u) {
                call += ", gradient2d(float2(" + numericArg(9, "0.0") +
                        ", 0.0f), float2(" + numericArg(12, "0.0") +
                        ", 0.0f))";
            } else if (is_3d_gradient) {
                call += ", gradient3d(float3(" + numericArg(9, "0.0") + ", " +
                       numericArg(10, "0.0") + ", " + numericArg(11, "0.0") +
                       "), float3(" + numericArg(12, "0.0") + ", " +
                       numericArg(13, "0.0") + ", " + numericArg(14, "0.0") + "))";
            } else {
                call += ", gradient2d(float2(" + numericArg(9, "0.0") + ", " +
                       numericArg(10, "0.0") + "), float2(" +
                       numericArg(12, "0.0") + ", " + numericArg(13, "0.0") + "))";
            }
        }
        if (has_offset) {
            if (resource_kind == 4u)
                call += ", int3(" + ox + ", " + oy + ", " + oz + ")";
            else
                call += ", int2(" + ox + ", " + oy + ")";
        }
        call += ")";
        auto replace_binding = [](std::string expression,
                                  const std::string &from,
                                  const std::string &to) {
            if (from.empty() || from == to)
                return expression;
            for (size_t position = expression.find(from);
                 position != std::string::npos;
                 position = expression.find(from, position + to.size()))
                expression.replace(position, from.size(), to);
            return expression;
        };
        auto dynamic_texture = ctx.resource_handles.find(args[0]);
        auto dynamic_sampler = ctx.resource_handles.find(args[1]);
        const bool has_dynamic_texture =
            dynamic_texture != ctx.resource_handles.end() &&
            !dynamic_texture->second.dynamic_index.empty() &&
            dynamic_texture->second.binding_count > 1;
        const bool has_dynamic_sampler =
            dynamic_sampler != ctx.resource_handles.end() &&
            !dynamic_sampler->second.dynamic_index.empty() &&
            dynamic_sampler->second.binding_count > 1;
        auto call_for = [&](int texture_slot, int sampler_slot) {
            std::string selected_call = call;
            if (texture_slot >= 0)
                selected_call = replace_binding(
                    selected_call, handle,
                    "tex" + std::to_string(texture_slot));
            if (sampler_slot >= 0)
                selected_call = replace_binding(
                    selected_call, samp,
                    "samp" + std::to_string(sampler_slot));
            return selected_call;
        };
        auto select_sampler = [&](int texture_slot) {
            if (!has_dynamic_sampler)
                return call_for(texture_slot, -1);
            const uint32_t base = dynamic_sampler->second.lower_bound;
            const uint32_t count = std::min<uint32_t>(
                dynamic_sampler->second.binding_count,
                ctx.binding_plan.direct_sampler_count > base
                    ? ctx.binding_plan.direct_sampler_count - base
                    : 0);
            if (count <= 1)
                return call_for(texture_slot, -1);
            std::string selected =
                call_for(texture_slot, static_cast<int>(base + count - 1));
            for (uint32_t i = count - 1; i > 0; --i) {
                selected = "((uint(" + dynamic_sampler->second.dynamic_index +
                           ") == " + std::to_string(base + i - 1) +
                           "u) ? " +
                           call_for(texture_slot,
                                    static_cast<int>(base + i - 1)) +
                           " : " + selected + ")";
            }
            return selected;
        };
        if (has_dynamic_texture) {
            const uint32_t base = dynamic_texture->second.lower_bound;
            const uint32_t count = std::min<uint32_t>(
                dynamic_texture->second.binding_count,
                ctx.binding_plan.direct_texture_count > base
                    ? ctx.binding_plan.direct_texture_count - base
                    : 0);
            if (count > 1) {
                std::string selected = select_sampler(
                    static_cast<int>(base + count - 1));
                for (uint32_t i = count - 1; i > 0; --i) {
                    selected = "((uint(" +
                               dynamic_texture->second.dynamic_index +
                               ") == " + std::to_string(base + i - 1) +
                               "u) ? " +
                               select_sampler(
                                   static_cast<int>(base + i - 1)) +
                               " : " + selected + ")";
                }
                return "float4(" + selected + ")";
            }
        }
        if (has_dynamic_sampler)
            return "float4(" + select_sampler(-1) + ")";
        return call;
    }
    case DXOP_TextureGather: case DXOP_TextureGatherCmp: case 223: {
        if (args.size() < 4) return "float4(0)";
        auto handle = handleArg(0, "tex", "tex0");
        auto samp = handleArg(1, "samp", "samp0");
        const uint32_t resource_kind = resourceKindForHandle(ctx, args[0]);
        auto cx = sampleCoordComponent(ctx, valueArg(2, "0.0"), 0);
        auto cy = sampleCoordComponent(ctx, valueArg(3, "0.0"), 1);
        auto c2 = sampleCoordComponent(ctx, valueArg(4, "0.0"), 2);
        auto ox = ensureScalarIndex(numericArg(6, "0"));
        auto oy = ensureScalarIndex(numericArg(7, "0"));
        if (resource_kind != 0u && resource_kind != 2u && resource_kind != 7u) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL texture gather is unsupported for resource kind=%u", resource_kind);
            return "float4(0)";
        }
        const bool array_texture = resource_kind == 7u;
        const std::string coord = "float2(" + cx + ", " + cy + ")";
        const std::string array_suffix = array_texture ? ", (uint)(" + c2 + ")" : "";
        std::string call;
        if (intrinsic_id == DXOP_TextureGatherCmp) {
            auto compare = numericArg(9, "0.0");
            call = handle + ".gather_compare(" + samp + ", " + coord +
                   array_suffix + ", (float)(" + compare + "), int2(" + ox +
                   ", " + oy + "))";
        } else if (intrinsic_id == DXOP_TextureGatherRaw ||
                   intrinsic_id == 223) {
            call = handle + ".gather(" + samp + ", " + coord + array_suffix +
                   ", int2(" + ox + ", " + oy + "), component::x)";
        } else {
            uint32_t ch = args.size() > 8 ? literalArg(8, 0, "ch") : 0;
            call = handle + ".gather(" + samp + ", " + coord + array_suffix +
                   ", int2(" + ox + ", " + oy + "), component::" +
                   componentName(ch) + ")";
        }
        auto dynamic_texture = ctx.resource_handles.find(args[0]);
        auto dynamic_sampler = ctx.resource_handles.find(args[1]);
        const bool has_dynamic_texture =
            dynamic_texture != ctx.resource_handles.end() &&
            !dynamic_texture->second.dynamic_index.empty() &&
            dynamic_texture->second.binding_count > 1;
        const bool has_dynamic_sampler =
            dynamic_sampler != ctx.resource_handles.end() &&
            !dynamic_sampler->second.dynamic_index.empty() &&
            dynamic_sampler->second.binding_count > 1;
        auto call_for = [&](int texture_slot, int sampler_slot) {
            std::string selected = call;
            if (texture_slot >= 0) {
                const std::string replacement =
                    "tex" + std::to_string(texture_slot);
                const size_t position = selected.find(handle);
                if (position != std::string::npos)
                    selected.replace(position, handle.size(), replacement);
            }
            if (sampler_slot >= 0) {
                const std::string replacement =
                    "samp" + std::to_string(sampler_slot);
                const size_t position = selected.find(samp);
                if (position != std::string::npos)
                    selected.replace(position, samp.size(), replacement);
            }
            return selected;
        };
        auto select_sampler = [&](int texture_slot) {
            if (!has_dynamic_sampler)
                return call_for(texture_slot, -1);
            const uint32_t base = dynamic_sampler->second.lower_bound;
            const uint32_t count = std::min<uint32_t>(
                dynamic_sampler->second.binding_count,
                ctx.binding_plan.direct_sampler_count > base
                    ? ctx.binding_plan.direct_sampler_count - base
                    : 0);
            if (count <= 1)
                return call_for(texture_slot, -1);
            std::string selected =
                call_for(texture_slot, static_cast<int>(base + count - 1));
            for (uint32_t i = count - 1; i > 0; --i)
                selected = "((uint(" + dynamic_sampler->second.dynamic_index +
                           ") == " + std::to_string(base + i - 1) +
                           "u) ? " +
                           call_for(texture_slot,
                                    static_cast<int>(base + i - 1)) +
                           " : " + selected + ")";
            return selected;
        };
        if (has_dynamic_texture) {
            const uint32_t base = dynamic_texture->second.lower_bound;
            const uint32_t count = std::min<uint32_t>(
                dynamic_texture->second.binding_count,
                ctx.binding_plan.direct_texture_count > base
                    ? ctx.binding_plan.direct_texture_count - base
                    : 0);
            if (count > 1) {
                std::string selected =
                    select_sampler(static_cast<int>(base + count - 1));
                for (uint32_t i = count - 1; i > 0; --i)
                    selected = "((uint(" +
                               dynamic_texture->second.dynamic_index +
                               ") == " + std::to_string(base + i - 1) +
                               "u) ? " +
                               select_sampler(static_cast<int>(base + i - 1)) +
                               " : " + selected + ")";
                return "float4(" + selected + ")";
            }
        }
        if (has_dynamic_sampler)
            return "float4(" + select_sampler(-1) + ")";
        return call;
    }
    case 8:
        return "isnan(" + numericArg(0, "0.0") + ")";
    case 9:
        return "isinf(" + numericArg(0, "0.0") + ")";
    case 10:
        return "isfinite(" + numericArg(0, "0.0") + ")";
    case 11: {
        auto value = numericArg(0, "0.0");
        const char *minimum = callee_name.find(".f16") != std::string::npos
                                  ? "0.00006103515625"
                                  : "1.17549435e-38";
        return "(isfinite(" + value + ") && (" + value + ") != 0.0 && abs(" +
               value + ") >= " + minimum + ")";
    }
    case DXOP_TextureSampleCmp:
    case DXOP_TextureSampleCmpLevelZero:
    case DXOP_TextureSampleCmpGrad:
    case DXOP_TextureSampleCmpBias:
    case 224: {
        if (args.size() < 10) return "0.0";
        auto handle = handleArg(0, "tex", "tex0");
        auto samp = handleArg(1, "samp", "samp0");
        const uint32_t resource_kind = resourceKindForHandle(ctx, args[0]);
        auto c0 = sampleCoordComponent(ctx, valueArg(2, "0.0"), 0);
        auto c1 = sampleCoordComponent(ctx, valueArg(3, "0.0"), 1);
        auto c2 = sampleCoordComponent(ctx, valueArg(4, "0.0"), 2);
        auto c3 = sampleCoordComponent(ctx, valueArg(5, "0.0"), 3);
        auto cmp = valueArg(9, "0.0");
        auto ox = ensureScalarIndex(numericArg(6, "0"));
        auto oy = ensureScalarIndex(numericArg(7, "0"));
        if (resource_kind != 0u && resource_kind != 1u && resource_kind != 2u &&
            resource_kind != 5u && resource_kind != 6u && resource_kind != 7u &&
            resource_kind != 9u) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL comparison sample is unsupported for resource kind=%u", resource_kind);
            return "0.0";
        }

        std::string coord;
        std::string array_suffix;
        bool has_offset = true;
        switch (resource_kind) {
        case 1u:
            coord = "float2(" + c0 + ", 0.5f)";
            has_offset = true;
            break;
        case 6u:
            coord = "float2(" + c0 + ", 0.5f)";
            array_suffix = ", (uint)(" + c1 + ")";
            break;
        case 2u:
            coord = "float2(" + c0 + ", " + c1 + ")";
            break;
        case 7u:
            coord = "float2(" + c0 + ", " + c1 + ")";
            array_suffix = ", (uint)(" + c2 + ")";
            break;
        case 5u:
            coord = "float3(" + c0 + ", " + c1 + ", " + c2 + ")";
            has_offset = false;
            break;
        case 9u:
            coord = "float3(" + c0 + ", " + c1 + ", " + c2 + ")";
            array_suffix = ", (uint)(" + c3 + ")";
            has_offset = false;
            break;
        default:
            coord = "float2(" + c0 + ", " + c1 + ")";
            break;
        }
        if (ctx.shader.kind == DxilShaderKind::Compute &&
            (resource_kind == 1u || resource_kind == 6u)) {
            std::string sample;
            if (resource_kind == 1u)
                sample = handle + ".read(uint2((uint)(" + c0 + "), 0u), (uint)(" + numericArg(10, "0") + "))";
            else if (resource_kind == 6u)
                sample = handle + ".read(uint2((uint)(" + c0 + "), 0u), (uint)(" + c1 + "), (uint)(" + numericArg(10, "0") + "))";
            else
                sample = handle + ".read(uint2((uint)(" + c0 + "), (uint)(" + c1 + "))" +
                        (resource_kind == 7u ? ", (uint)(" + c2 + ")" : "") +
                        ((intrinsic_id == DXOP_TextureSampleCmpLevel || intrinsic_id == 224)
                             ? ", (uint)(" + numericArg(10, "0") + ")" : "") + ")";
            const std::string sampled_value =
                resource_kind == 0u || resource_kind == 2u ||
                        resource_kind == 7u
                    ? sample
                    : "(" + sample + ").x";
            return "(((float)(" + cmp + ") <= " + sampled_value +
                   ") ? 1.0f : 0.0f)";
        }
        std::string call = handle + ".sample_compare(" + samp + ", " + coord + array_suffix +
                          ", (float)(" + cmp + ")";
        if (intrinsic_id == DXOP_TextureSampleCmpGrad) {
            call += ", gradient2d(float2(" + numericArg(10, "0.0") + ", " +
                    numericArg(11, "0.0") + "), float2(" + numericArg(13, "0.0") +
                    ", " + numericArg(14, "0.0") + "))";
        } else if (intrinsic_id == DXOP_TextureSampleCmpBias) {
            call += ", bias((float)(" + numericArg(10, "0.0") + "))";
        } else if (intrinsic_id == DXOP_TextureSampleCmpLevel || intrinsic_id == 224) {
            call += ", level((float)(" + numericArg(10, "0.0") + "))";
        } else if (intrinsic_id == DXOP_TextureSampleCmpLevelZero) {
            call += ", level(0.0f)";
        }
        if (has_offset) {
            if (resource_kind == 1u || resource_kind == 6u)
                call += ", int2(" + ox + ", 0)";
            else
                call += ", int2(" + ox + ", " + oy + ")";
        }
        call += ")";
        auto dynamic_texture = ctx.resource_handles.find(args[0]);
        auto dynamic_sampler = ctx.resource_handles.find(args[1]);
        const bool has_dynamic_texture =
            dynamic_texture != ctx.resource_handles.end() &&
            !dynamic_texture->second.dynamic_index.empty() &&
            dynamic_texture->second.binding_count > 1;
        const bool has_dynamic_sampler =
            dynamic_sampler != ctx.resource_handles.end() &&
            !dynamic_sampler->second.dynamic_index.empty() &&
            dynamic_sampler->second.binding_count > 1;
        auto call_for = [&](int texture_slot, int sampler_slot) {
            std::string selected_call = call;
            if (texture_slot >= 0) {
                const size_t position = selected_call.find(handle);
                if (position != std::string::npos)
                    selected_call.replace(position, handle.size(),
                                          "tex" +
                                              std::to_string(texture_slot));
            }
            if (sampler_slot >= 0) {
                const size_t position = selected_call.find(samp);
                if (position != std::string::npos)
                    selected_call.replace(position, samp.size(),
                                          "samp" +
                                              std::to_string(sampler_slot));
            }
            return selected_call;
        };
        auto select_sampler = [&](int texture_slot) {
            if (!has_dynamic_sampler)
                return call_for(texture_slot, -1);
            const uint32_t base = dynamic_sampler->second.lower_bound;
            const uint32_t count = std::min<uint32_t>(
                dynamic_sampler->second.binding_count,
                ctx.binding_plan.direct_sampler_count > base
                    ? ctx.binding_plan.direct_sampler_count - base
                    : 0);
            if (count <= 1)
                return call_for(texture_slot, -1);
            std::string selected =
                call_for(texture_slot, static_cast<int>(base + count - 1));
            for (uint32_t i = count - 1; i > 0; --i)
                selected = "((uint(" + dynamic_sampler->second.dynamic_index +
                           ") == " + std::to_string(base + i - 1) +
                           "u) ? " +
                           call_for(texture_slot,
                                    static_cast<int>(base + i - 1)) +
                           " : " + selected + ")";
            return selected;
        };
        if (has_dynamic_texture) {
            const uint32_t base = dynamic_texture->second.lower_bound;
            const uint32_t count = std::min<uint32_t>(
                dynamic_texture->second.binding_count,
                ctx.binding_plan.direct_texture_count > base
                    ? ctx.binding_plan.direct_texture_count - base
                    : 0);
            if (count > 1) {
                std::string selected =
                    select_sampler(static_cast<int>(base + count - 1));
                for (uint32_t i = count - 1; i > 0; --i)
                    selected = "((uint(" +
                               dynamic_texture->second.dynamic_index +
                               ") == " + std::to_string(base + i - 1) +
                               "u) ? " +
                               select_sampler(static_cast<int>(base + i - 1)) +
                               " : " + selected + ")";
                return selected;
            }
        }
        if (has_dynamic_sampler)
            return select_sampler(-1);
        return call;
    }
    case DXOP_BufferUpdateCounter: {
        uint32_t counter_bindings = 0;
        for (const auto &binding : ctx.mod.resource_bindings)
            if (binding.resource_class == 1 && binding.has_counter)
                ++counter_bindings;
        const uint32_t reserved_srv_register =
            ctx.shader.kind == DxilShaderKind::Compute ? 14u : 9u;
        bool reserved_srv_used = false;
        for (const auto &range : ctx.binding_plan.ranges)
            if (range.kind == DescriptorRangePlan::Kind::SRV &&
                range.register_space == 0 &&
                range.lower_bound <= reserved_srv_register &&
                range.count > reserved_srv_register - range.lower_bound)
                reserved_srv_used = true;
        if ((ctx.shader.kind != DxilShaderKind::Compute &&
             ctx.shader.kind != DxilShaderKind::Vertex &&
             ctx.shader.kind != DxilShaderKind::Pixel) ||
            counter_bindings != 1 || reserved_srv_used ||
            ctx.options.resource_heap_directly_indexed) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(
                ctx,
                "DXIL buffer counter requires compute/vertex/pixel stage, exactly one table-bound counter UAV, and a free reserved SRV slot (stage=%u counters=%u reserved_t=%u occupied=%u direct_heap=%u)",
                static_cast<unsigned>(ctx.shader.kind), counter_bindings,
                reserved_srv_register, reserved_srv_used ? 1u : 0u,
                ctx.options.resource_heap_directly_indexed ? 1u : 0u);
            return "0";
        }
        auto delta = ensureScalarIndex(numericArg(1, "1"));
        const char *counter_buffer =
            ctx.shader.kind == DxilShaderKind::Compute ? "buf30" : "buf25";
        return "m12_update_counter(reinterpret_cast<device atomic_uint*>(" +
               std::string(counter_buffer) + "), (int)(" + delta + "))";
    }
    case DXOP_CheckAccessFullyMapped:
        return "true";
    case 72: {
        auto handle = handleArg(0, "tex", "tex0");
        const uint32_t resource_kind = resourceKindForHandle(ctx, args.empty() ? 0u : args[0]);
        auto mip = args.size() > 1 ? ensureScalarIndex(numericArg(1, "0")) : "0";
        const bool multisample = resource_kind == 3u || resource_kind == 8u;
        std::string width = multisample
                                ? handle + ".get_width()"
                                : handle + ".get_width((uint)(" + mip + "))";
        std::string height = "1u";
        if (resource_kind == 2u || resource_kind == 3u ||
            resource_kind == 4u || resource_kind == 5u ||
            resource_kind == 7u || resource_kind == 8u ||
            resource_kind == 9u)
            height = multisample ? handle + ".get_height()"
                                 : handle + ".get_height((uint)(" + mip + "))";
        std::string array_size = isTextureArrayResourceKind(resource_kind)
                                     ? handle + ".get_array_size()"
                                     : "1u";
        std::string depth = resource_kind == 4u
                                ? handle + ".get_depth((uint)(" + mip + "))"
                                : "1u";
        std::string samples = multisample ? handle + ".get_num_samples()" : "1u";
        std::string mip_levels = multisample
                                     ? "1u"
                                     : handle + ".get_num_mip_levels()";
        // GetDimensions returns only the components applicable to the DXIL
        // resource shape: 1D (w[,array]), 2D (w,h[,array]), 3D (w,h,d),
        // cube (w,h[,array]), and MSAA (w,h[,array],samples).  The lowerer
        // uses a four-lane carrier, so place each value in the component that
        // the corresponding DXIL extractvalue consumes.
        if (resource_kind == 1u)
            return "uint4(" + width + ", 1u, 1u, " + mip_levels + ")";
        if (resource_kind == 6u)
            return "uint4(" + width + ", " + array_size + ", 1u, " +
                   mip_levels + ")";
        if (resource_kind == 3u)
            return "uint4(" + width + ", " + height + ", 1u, " + samples + ")";
        if (resource_kind == 8u)
            return "uint4(" + width + ", " + height + ", " + array_size +
                   ", " + samples + ")";
        if (resource_kind == 4u)
            return "uint4(" + width + ", " + height + ", " + depth + ", " +
                   mip_levels + ")";
        if (resource_kind == 7u || resource_kind == 9u)
            return "uint4(" + width + ", " + height + ", " + array_size +
                   ", " + mip_levels + ")";
        return "uint4(" + width + ", " + height + ", 1u, " + mip_levels +
               ")";
    }
    case 83: case 85: return "dfdx(" + valueArg(0, "0.0") + ")";
    case 84: case 86: return "dfdy(" + valueArg(0, "0.0") + ")";
    case 81: {
        if (args.size() < 6) return "0.0";
        auto handle = handleArg(0, "tex", "tex0");
        auto samp = handleArg(1, "samp", "samp0");
        const uint32_t resource_kind = resourceKindForHandle(ctx, args[0]);
        auto c0 = sampleCoordComponent(ctx, valueArg(2, "0.0"), 0);
        auto c1 = sampleCoordComponent(ctx, valueArg(3, "0.0"), 1);
        auto c2 = sampleCoordComponent(ctx, valueArg(4, "0.0"), 2);
        std::string coord;
        if (resource_kind == 1u || resource_kind == 6u)
            coord = "float2(" + c0 + ", 0.5f)";
        else if (resource_kind == 2u || resource_kind == 7u ||
                 resource_kind == 0u)
            coord = "float2(" + c0 + ", " + c1 + ")";
        else if (resource_kind == 4u || resource_kind == 5u ||
                 resource_kind == 9u)
            coord = "float3(" + c0 + ", " + c1 + ", " + c2 + ")";
        else {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx,
                             "DXIL calculate LOD is unsupported for resource kind=%u",
                             resource_kind);
            return "0.0";
        }
        const bool clamped = literalArg(args.size() - 1, 0, "clamped") != 0;
        return handle + (clamped ? ".calculate_clamped_lod("
                                 : ".calculate_unclamped_lod(") +
               samp + ", " + coord + ")";
    }
    case 78: {
        if (args.size() < 4) return "0";
        auto handle = handleArg(0, "buf", "buf0");
        auto op = literalArg(1, 0, "atomic_op");
        auto coordinate = ensureScalarIndex(numericArg(2, "0"));
        auto element_offset = ensureScalarIndex(numericArg(3, "0"));
        auto val = ensureScalarIndex(numericArg(args.size() - 1, "0"));
        if (callee_name.find(".i64") != std::string::npos) {
            uint32_t resource_kind = 0;
            uint32_t element_stride = 0;
            auto handle_it = ctx.resource_handles.find(args[0]);
            if (handle_it != ctx.resource_handles.end()) {
                resource_kind = handle_it->second.resource_kind;
                element_stride = handle_it->second.element_stride;
            }
            std::string byte_offset;
            if (resource_kind == 12u) {
                if (element_stride == 0)
                    element_stride = 8;
                byte_offset = "((ulong)(" + coordinate + ") * " +
                              std::to_string(element_stride) + "ul + (ulong)(" +
                              element_offset + "))";
            } else if (resource_kind == 10u) {
                byte_offset = "((ulong)(" + coordinate +
                              ") * 8ul + (ulong)(" + element_offset + "))";
            } else {
                byte_offset = "((ulong)(" + coordinate + ") + (ulong)(" +
                              element_offset + "))";
            }
            ctx.uses_atomic64_emulation = true;
            return "(long)m12_atomic64_binop(reinterpret_cast<volatile device ulong*>(" +
                   handle + " + " + byte_offset + "), (ulong)(" + val +
                   "), " + std::to_string(op) + "u, m12_atomic64_lock)";
        }
        uint32_t resource_kind = 0;
        uint32_t element_stride = 0;
        auto handle_it = ctx.resource_handles.find(args[0]);
        if (handle_it != ctx.resource_handles.end()) {
            resource_kind = handle_it->second.resource_kind;
            element_stride = handle_it->second.element_stride;
        }
        std::string byte_offset;
        if (resource_kind == 12u) {
            if (element_stride == 0)
                element_stride = 4;
            byte_offset = "((ulong)(" + coordinate + ") * " +
                          std::to_string(element_stride) + "ul + (ulong)(" +
                          element_offset + "))";
        } else if (resource_kind == 10u) {
            if (element_stride == 0)
                element_stride = 4;
            byte_offset = "((ulong)(" + coordinate + ") * " +
                          std::to_string(element_stride) + "ul)";
        } else {
            byte_offset = "((ulong)(" + coordinate + ") + (ulong)(" +
                          element_offset + "))";
        }
        ctx.uses_atomic32_emulation = true;
        return "m12_atomic32_binop(reinterpret_cast<device atomic_uint*>(" +
               handle + " + " + byte_offset + "), (uint)(" + val + "), " +
               std::to_string(op) + "u)";
    }
    case 79: {
        if (args.size() < 2) return "0";
        auto handle = handleArg(0, "buf", "buf0");
        if (callee_name.find(".i64") != std::string::npos && args.size() >= 6) {
            auto coordinate = ensureScalarIndex(numericArg(1, "0"));
            auto element_offset = ensureScalarIndex(numericArg(2, "0"));
            auto compare_value = ensureScalarIndex(numericArg(4, "0"));
            auto new_value = ensureScalarIndex(numericArg(5, "0"));
            uint32_t resource_kind = 0;
            uint32_t element_stride = 0;
            auto handle_it = ctx.resource_handles.find(args[0]);
            if (handle_it != ctx.resource_handles.end()) {
                resource_kind = handle_it->second.resource_kind;
                element_stride = handle_it->second.element_stride;
            }
            std::string byte_offset;
            if (resource_kind == 12u) {
                if (element_stride == 0)
                    element_stride = 8;
                byte_offset = "((ulong)(" + coordinate + ") * " +
                              std::to_string(element_stride) + "ul + (ulong)(" +
                              element_offset + "))";
            } else if (resource_kind == 10u) {
                byte_offset = "((ulong)(" + coordinate +
                              ") * 8ul + (ulong)(" + element_offset + "))";
            } else {
                byte_offset = "((ulong)(" + coordinate + ") + (ulong)(" +
                              element_offset + "))";
            }
            ctx.uses_atomic64_emulation = true;
            return "(long)m12_atomic64_compare_exchange(reinterpret_cast<volatile device ulong*>(" +
                   handle + " + " + byte_offset + "), (ulong)(" +
                   compare_value + "), (ulong)(" + new_value +
                   "), m12_atomic64_lock)";
        }
        auto coordinate = ensureScalarIndex(numericArg(1, "0"));
        auto element_offset = ensureScalarIndex(numericArg(2, "0"));
        auto compare_value = ensureScalarIndex(numericArg(4, "0"));
        auto new_value = ensureScalarIndex(numericArg(5, "0"));
        uint32_t resource_kind = 0;
        uint32_t element_stride = 0;
        auto handle_it = ctx.resource_handles.find(args[0]);
        if (handle_it != ctx.resource_handles.end()) {
            resource_kind = handle_it->second.resource_kind;
            element_stride = handle_it->second.element_stride;
        }
        std::string byte_offset;
        if (resource_kind == 12u) {
            if (element_stride == 0)
                element_stride = 4;
            byte_offset = "((ulong)(" + coordinate + ") * " +
                          std::to_string(element_stride) + "ul + (ulong)(" +
                          element_offset + "))";
        } else if (resource_kind == 10u) {
            if (element_stride == 0)
                element_stride = 4;
            byte_offset = "((ulong)(" + coordinate + ") * " +
                          std::to_string(element_stride) + "ul)";
        } else {
            byte_offset = "((ulong)(" + coordinate + ") + (ulong)(" +
                          element_offset + "))";
        }
        ctx.uses_atomic32_emulation = true;
        return "m12_atomic32_compare_exchange(reinterpret_cast<device atomic_uint*>(" +
               handle + " + " + byte_offset + "), (uint)(" + compare_value +
               "), (uint)(" + new_value + "))";
    }
    case DXOP_TempRegLoad:
    case DXOP_MinPrecXRegLoad: {
        if (args.empty())
            return "0";
        MSLType temp_type = {MSLTypeKind::Int, 0, {}};
        if (callee_name.find(".i1") != std::string::npos)
            temp_type = {MSLTypeKind::Bool, 0, {}};
        else if (callee_name.find(".f16") != std::string::npos)
            temp_type = {MSLTypeKind::Half, 0, {}};
        else if (callee_name.find(".f32") != std::string::npos)
            temp_type = {MSLTypeKind::Float, 0, {}};
        if (intrinsic_id == DXOP_MinPrecXRegLoad) {
            if (args.size() < 3)
                return "0";
            const std::string base = resolveValue(ctx, args[0]);
            if (base.empty() || startsWith(base, "dx.") ||
                !exprContainsPointerSyntax(base)) {
                ctx.unsupported_intrinsics++;
                recordDiagnostic(ctx, "DXIL min-precision register load has no pointer base");
                return "0";
            }
            const std::string index = numericArg(1, "0");
            const std::string component = numericArg(2, "0");
            const char *address_space =
                base.find("threadgroup") != std::string::npos ? "threadgroup" : "thread";
            const std::string lane = "((" + index + ") * 4u + (" + component + "))";
            const std::string lvalue =
                "*reinterpret_cast<" + std::string(address_space) +
                " float*>(" + base + " + (" + lane + ") * 4u)";
            return temp_type.kind == MSLTypeKind::Half
                       ? "half(" + lvalue + ")"
                       : lvalue;
        }
        const char *storage = tempRegisterStorage(temp_type);
        const std::string index = "min(uint(" + numericArg(0, "0") + "), 4095u)";
        if (temp_type.kind == MSLTypeKind::Half)
            return std::string("half(") + storage + "[" + index + "])";
        return std::string(storage) + "[" + index + "]";
    }
    case DXOP_TempRegStore:
    case DXOP_MinPrecXRegStore: {
        if (args.size() < 2)
            return {};
        MSLType temp_type = {MSLTypeKind::Int, 0, {}};
        if (callee_name.find(".i1") != std::string::npos)
            temp_type = {MSLTypeKind::Bool, 0, {}};
        else if (callee_name.find(".f16") != std::string::npos)
            temp_type = {MSLTypeKind::Half, 0, {}};
        else if (callee_name.find(".f32") != std::string::npos)
            temp_type = {MSLTypeKind::Float, 0, {}};
        if (intrinsic_id == DXOP_MinPrecXRegStore) {
            if (args.size() < 4)
                return {};
            const std::string base = resolveValue(ctx, args[0]);
            if (base.empty() || startsWith(base, "dx.") ||
                !exprContainsPointerSyntax(base)) {
                ctx.unsupported_intrinsics++;
                recordDiagnostic(ctx, "DXIL min-precision register store has no pointer base");
                return {};
            }
            const std::string index = numericArg(1, "0");
            const std::string component = numericArg(2, "0");
            const char *address_space =
                base.find("threadgroup") != std::string::npos ? "threadgroup" : "thread";
            const std::string lane = "((" + index + ") * 4u + (" + component + "))";
            const std::string lvalue =
                "*reinterpret_cast<" + std::string(address_space) +
                " float*>(" + base + " + (" + lane + ") * 4u)";
            std::string value = numericArg(3, "0");
            if (temp_type.kind == MSLTypeKind::Half)
                value = "float(" + value + ")";
            return lvalue + " = " + value;
        }
        const char *storage = tempRegisterStorage(temp_type);
        const std::string index = "min(uint(" + numericArg(0, "0") + "), 4095u)";
        const size_t value_index = intrinsic_id == DXOP_MinPrecXRegStore
                                       ? args.size() - 1
                                       : 1;
        std::string value = numericArg(value_index, "0");
        if (temp_type.kind == MSLTypeKind::Half)
            value = "float(" + value + ")";
        return std::string(storage) + "[" + index + "] = " + value;
    }
    case 75:
    case 76: {
        if (ctx.shader.kind != DxilShaderKind::Pixel) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL sample-position intrinsic requires a pixel shader");
            return "float2(0.5)";
        }
        const size_t index_arg = intrinsic_id == 75 ? 1u : 0u;
        return "get_sample_position((uint)(" + numericArg(index_arg, "0") + "))";
    }
    case 77:
        if (ctx.shader.kind != DxilShaderKind::Pixel) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL render-target sample-count intrinsic requires a pixel shader");
            return "1u";
        }
        return "get_num_samples()";
    case DXOP_SampleIndex:
        if (ctx.shader.kind != DxilShaderKind::Pixel) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL sample-index intrinsic requires a pixel shader");
            return "0u";
        }
        return "m12_sample_id";
    case DXOP_Coverage:
        if (ctx.shader.kind != DxilShaderKind::Pixel) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL coverage intrinsic requires a pixel shader");
            return "0u";
        }
        return "m12_coverage";
    case DXOP_InnerCoverage:
        if (ctx.shader.kind == DxilShaderKind::Pixel &&
            ctx.options.conservative_rasterization)
            return "(m12_conservative.enabled != 0u && m12_cons_inner_triangle_pixel(m12_conservative.p0, m12_conservative.p1, m12_conservative.p2, floor(in.position.xy)) ? 1u : 0u)";
        ctx.unsupported_intrinsics++;
        recordDiagnostic(ctx,
                         "DXIL inner-coverage intrinsic requires the conservative raster reference provider");
        return "0u";
    case DXOP_EvalSnapped:
    case DXOP_EvalSampleIndex:
    case DXOP_EvalCentroid: {
        if (ctx.shader.kind != DxilShaderKind::Pixel || args.size() < 3) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL attribute evaluation requires a pixel shader");
            return "0.0f";
        }
        const uint32_t input_id = literalArg(0, 0, "input signature");
        const uint32_t component = literalArg(2, 0, "input component");
        std::string field = varyingField("in", input_id);
        std::string evaluated;
        if (input_id == 0) {
            evaluated = field;
        } else if (intrinsic_id == DXOP_EvalCentroid) {
            ctx.uses_interpolation = true;
            evaluated = field + ".interpolate_at_centroid()";
        } else if (intrinsic_id == DXOP_EvalSampleIndex && args.size() >= 4) {
            ctx.uses_interpolation = true;
            evaluated = field + ".interpolate_at_sample((uint)(" +
                        numericArg(3, "0") + "))";
        } else if (intrinsic_id == DXOP_EvalSnapped && args.size() >= 5) {
            ctx.uses_interpolation = true;
            evaluated = field + ".interpolate_at_offset(float2((float)(" +
                        numericArg(3, "0") + ") / 16.0f, (float)(" +
                        numericArg(4, "0") + ") / 16.0f))";
        } else {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL attribute evaluation has malformed operands");
            return "0.0f";
        }
        return evaluated + componentSuffix(component);
    }
    case 97:
    case 98:
        ctx.unsupported_intrinsics++;
        recordDiagnostic(ctx, "DXIL geometry stream intrinsic requires geometry-stage lowering");
        return "0";
    case 109:
        ctx.unsupported_intrinsics++;
        recordDiagnostic(ctx, "DXIL cycle-counter intrinsic is unsupported; rejecting shader");
        return "0";
    case 80: return "threadgroup_barrier(mem_flags::mem_threadgroup)";
    case DXOP_Discard:
        return "discard_fragment()";
    case DXOP_Unpack4x8: {
        if (args.size() < 2) return "uint4(0)";
        const bool signed_mode = literalArg(0, 0, "unpack mode") == 1u;
        const std::string packed = "uint(" + numericArg(1, "0") + ")";
        std::string values;
        for (unsigned i = 0; i < 4; ++i) {
            if (i) values += ", ";
            const std::string shift = std::to_string(i * 8) + "u";
            if (signed_mode)
                values += "(int(int((" + packed + " >> " + shift + ") & 0xffu) << 24) >> 24)";
            else
                values += "(" + packed + " >> " + shift + ") & 0xffu";
        }
        return (signed_mode ? "int4(" : "uint4(") + values + ")";
    }
    case DXOP_Pack4x8: {
        if (args.size() < 5) return "0u";
        const uint32_t mode = literalArg(0, 0, "pack mode");
        std::string packed = "0u";
        for (unsigned i = 0; i < 4; ++i) {
            std::string value = numericArg(i + 1, "0");
            if (mode == 1u)
                value = "min(max(uint(" + value + "), 0u), 255u)";
            else if (mode == 2u)
                value = "uint(min(max(int(" + value + "), -128), 127))";
            packed += " | ((uint(" + value + ") & 0xffu) << " +
                      std::to_string(i * 8) + "u)";
        }
        return packed;
    }
    case DXOP_VectorReduceAnd:
    case DXOP_VectorReduceOr: {
        if (args.empty()) return "false";
        const MSLType value_type = valueTypeOrUnknown(ctx, args[0]);
        const std::string value = DXILIRBuilder::isLongVectorType(value_type)
                                      ? vectorArg(0, "{}")
                                      : valueArg(0, "0");
        if (DXILIRBuilder::isLongVectorType(value_type)) {
            std::string result = intrinsic_id == DXOP_VectorReduceAnd
                                     ? "true"
                                     : "false";
            for (uint32_t i = value_type.vector_width; i-- > 0;) {
                const std::string term = "(" + value + "[" +
                                         std::to_string(i) + "] != 0)";
                result = intrinsic_id == DXOP_VectorReduceAnd
                             ? "(" + term + " && " + result + ")"
                             : "(" + term + " || " + result + ")";
            }
            return result;
        }
        const std::string predicate = "(" + value + " != 0)";
        return intrinsic_id == DXOP_VectorReduceAnd
                   ? "all(" + predicate + ")"
                   : "any(" + predicate + ")";
    }
    case DXOP_Unary: {
        if (args.size() < 2) return "0";
        uint32_t op = literalArg(0, 0xFFFFFFFFu, "unary");
        const MSLType operand_type = valueTypeOrUnknown(ctx, args[1]);
        if (DXILIRBuilder::isLongVectorType(operand_type)) {
            const std::string value = vectorArg(1, "{}");
            MSLType result_type = operand_type;
            const bool integer_result = op == DXILOP_Countbits ||
                                         op == DXILOP_FirstbitLo ||
                                         op == DXILOP_FirstbitHi ||
                                         op == DXILOP_FirstbitSHi;
            if (integer_result)
                result_type.vector_element_kind = MSLTypeKind::Int;
            std::string expression = emitTypeName(result_type) + "{{";
            for (uint32_t i = 0; i < operand_type.vector_width; ++i) {
                if (i)
                    expression += ", ";
                const std::string component = value + "[" +
                                              std::to_string(i) + "]";
                const std::string floating = "static_cast<float>(" +
                                             component + ")";
                switch (op) {
                case DXILOP_FAbs: expression += "abs(" + floating + ")"; break;
                case DXILOP_Saturate: expression += "clamp(" + floating + ", 0.0f, 1.0f)"; break;
                case DXILOP_IsNaN: expression += "isnan(" + floating + ") ? 1 : 0"; break;
                case DXILOP_IsInf: expression += "isinf(" + floating + ") ? 1 : 0"; break;
                case DXILOP_IsFinite: expression += "isfinite(" + floating + ") ? 1 : 0"; break;
                case DXILOP_IsNormal: expression += "isnormal(" + floating + ") ? 1 : 0"; break;
                case DXILOP_Cos: expression += "cos(" + floating + ")"; break;
                case DXILOP_Sin: expression += "sin(" + floating + ")"; break;
                case DXILOP_Tan: expression += "tan(" + floating + ")"; break;
                case DXILOP_Acos: expression += "acos(" + floating + ")"; break;
                case DXILOP_Asin: expression += "asin(" + floating + ")"; break;
                case DXILOP_Atan: expression += "atan(" + floating + ")"; break;
                case DXILOP_Hcos: expression += "cosh(" + floating + ")"; break;
                case DXILOP_Hsin: expression += "sinh(" + floating + ")"; break;
                case DXILOP_Htan: expression += "tanh(" + floating + ")"; break;
                case DXILOP_Exp: expression += "exp2(" + floating + ")"; break;
                case DXILOP_Frc: expression += "fract(" + floating + ")"; break;
                case DXILOP_Log: expression += "log2(" + floating + ")"; break;
                case DXILOP_Sqrt: expression += "sqrt(" + floating + ")"; break;
                case DXILOP_Rsqrt: expression += "rsqrt(" + floating + ")"; break;
                case DXILOP_Round_ne: expression += "rint(" + floating + ")"; break;
                case DXILOP_Round_ni: expression += "floor(" + floating + ")"; break;
                case DXILOP_Round_pi: expression += "ceil(" + floating + ")"; break;
                case DXILOP_Round_z: expression += "trunc(" + floating + ")"; break;
                case DXILOP_Bfrev: expression += "reverse_bits(uint(" + component + "))"; break;
                case DXILOP_Countbits: expression += "int(popcount(uint(" + component + ")))"; break;
                case DXILOP_FirstbitLo: expression += "((uint(" + component + ") == 0u) ? -1 : int(ctz(uint(" + component + "))))"; break;
                case DXILOP_FirstbitHi: expression += "((uint(" + component + ") == 0u) ? -1 : int(31u - clz(uint(" + component + "))))"; break;
                case DXILOP_FirstbitSHi: expression += "((int(" + component + ") < 0) ? ((~uint(" + component + ") == 0u) ? -1 : int(31u - clz(~uint(" + component + ")))) : ((uint(" + component + ") == 0u) ? -1 : int(31u - clz(uint(" + component + ")))))"; break;
                default: ctx.unsupported_intrinsics++; expression += "0"; break;
                }
            }
            expression += "}}";
            return expression;
        }
        bool int_op = op == DXILOP_Bfrev || op == DXILOP_Countbits ||
                      op == DXILOP_FirstbitLo || op == DXILOP_FirstbitHi ||
                      op == DXILOP_FirstbitSHi;
        const bool double_op = valueTypeOrUnknown(ctx, args[1]).kind == MSLTypeKind::Double;
        auto x = numericArg(1, int_op ? "0" : "0.0");
        auto dx = double_op ? doubleArg(1, "0") : x;
        auto fx = "static_cast<float>(" + x + ")";
        if (double_op && op != DXILOP_FAbs && op != DXILOP_Saturate &&
            op != DXILOP_IsNaN && op != DXILOP_IsInf &&
            op != DXILOP_IsFinite && op != DXILOP_IsNormal &&
            op != DXILOP_Sqrt && op != DXILOP_Rsqrt) {
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx,
                             "binary64 unary intrinsic requires software lowering op=%u",
                             op);
            return "ulong(0)";
        }
        switch (op) {
        case DXILOP_FAbs:
            return double_op ? "(ulong(" + dx + ") & 0x7ffffffffffffffful)" : "abs(" + fx + ")";
        case DXILOP_Saturate:
            if (double_op)
                return "(m12_f64_cmp(ulong(" + dx + "), 0ul, 4u) ? 0ul : (m12_f64_cmp(ulong(" + dx + "), 0x3ff0000000000000ul, 2u) ? 0x3ff0000000000000ul : ulong(" + dx + ")))";
            return "clamp(" + fx + ", 0.0, 1.0)";
        case DXILOP_IsNaN:
            return double_op ? "(((ulong(" + dx + ") >> 52) & 0x7fful) == 0x7fful && (ulong(" + dx + ") & 0x000ffffffffffffful) != 0ul)" : "isnan(" + fx + ")";
        case DXILOP_IsInf:
            return double_op ? "((ulong(" + dx + ") & 0x7ffffffffffffffful) == 0x7ff0000000000000ul)" : "isinf(" + fx + ")";
        case DXILOP_IsFinite:
            return double_op ? "(((ulong(" + dx + ") >> 52) & 0x7fful) != 0x7fful)" : "isfinite(" + fx + ")";
        case DXILOP_IsNormal:
            return double_op ? "((((ulong(" + dx + ") >> 52) & 0x7fful) != 0ul) && (((ulong(" + dx + ") >> 52) & 0x7fful) != 0x7fful))" : "isnormal(" + fx + ")";
        case DXILOP_Sqrt:
            return double_op ? "m12_f64_sqrt(ulong(" + dx + "))" : "sqrt(" + fx + ")";
        case DXILOP_Rsqrt:
            return double_op ? "m12_f64_div(0x3ff0000000000000ul, m12_f64_sqrt(ulong(" + dx + ")))" : "rsqrt(" + fx + ")";
        case DXILOP_Frc:
            return double_op ? "m12_f64_frac(ulong(" + dx + "))" : "fract(" + fx + ")";
        case DXILOP_Round_ne:
            return double_op ? "m12_f64_round_ne(ulong(" + dx + "))" : "rint(" + fx + ")";
        case DXILOP_Round_ni:
            return double_op ? "m12_f64_floor(ulong(" + dx + "))" : "floor(" + fx + ")";
        case DXILOP_Round_pi:
            return double_op ? "m12_f64_ceil(ulong(" + dx + "))" : "ceil(" + fx + ")";
        case DXILOP_Round_z:
            return double_op ? "m12_f64_trunc(ulong(" + dx + "))" : "trunc(" + fx + ")";
        case DXILOP_Cos: return "cos(" + fx + ")";
        case DXILOP_Sin: return "sin(" + fx + ")";
        case DXILOP_Tan: return "tan(" + fx + ")";
        case DXILOP_Acos: return "acos(" + fx + ")";
        case DXILOP_Asin: return "asin(" + fx + ")";
        case DXILOP_Atan: return "atan(" + fx + ")";
        case DXILOP_Hcos: return "cosh(" + fx + ")";
        case DXILOP_Hsin: return "sinh(" + fx + ")";
        case DXILOP_Htan: return "tanh(" + fx + ")";
        case DXILOP_Exp: return "exp2(" + fx + ")";
        case DXILOP_Log: return "log2(" + fx + ")";
        case DXILOP_Bfrev: return "reverse_bits(" + x + ")";
        case DXILOP_Countbits: return "popcount(static_cast<uint>(" + x + "))";
        case DXILOP_FirstbitLo:
            return "((uint(" + x + ") == 0u) ? -1 : int(ctz(uint(" + x + "))))";
        case DXILOP_FirstbitHi:
            return "((uint(" + x + ") == 0u) ? -1 : int(31u - clz(uint(" + x + "))))";
        case DXILOP_FirstbitSHi:
            return "((" + x + ") < 0 ? ((~uint(" + x + ") == 0u) ? -1 : int(31u - clz(~uint(" + x + ")))) : ((uint(" + x + ") == 0u) ? -1 : int(31u - clz(uint(" + x + ")))))";
        default: ctx.unsupported_intrinsics++; return x;
        }
    }
    case DXOP_Binary: {
        if (args.size() < 3) return "0";
        uint32_t op = literalArg(0, 0xFFFFFFFFu, "binary");
        const MSLType lhs_type = valueTypeOrUnknown(ctx, args[1]);
        const MSLType rhs_type = valueTypeOrUnknown(ctx, args[2]);
        const MSLType vector_type = DXILIRBuilder::isLongVectorType(lhs_type)
                                        ? lhs_type
                                        : rhs_type;
        if (DXILIRBuilder::isLongVectorType(vector_type)) {
            const std::string lhs = vectorArg(1, "{}");
            const std::string rhs = vectorArg(2, "{}");
            MSLType result_type = vector_type;
            if (op == DXILOP_IMax || op == DXILOP_IMin ||
                op == DXILOP_IMul)
                result_type.vector_element_kind = MSLTypeKind::Int;
            else if (op == DXILOP_UMax || op == DXILOP_UMin ||
                     op == DXILOP_UMul || op == DXILOP_UDiv ||
                     op == DXILOP_UAddc || op == DXILOP_USubb)
                result_type.vector_element_kind = MSLTypeKind::UInt;
            std::string expression = emitTypeName(result_type) + "{{";
            for (uint32_t i = 0; i < result_type.vector_width; ++i) {
                if (i)
                    expression += ", ";
                const std::string a = lhs + "[" + std::to_string(i) + "]";
                const std::string b = rhs + "[" + std::to_string(i) + "]";
                switch (op) {
                case DXILOP_FMax: expression += "max(" + a + ", " + b + ")"; break;
                case DXILOP_FMin: expression += "min(" + a + ", " + b + ")"; break;
                case DXILOP_IMax: expression += "max(int(" + a + "), int(" + b + "))"; break;
                case DXILOP_IMin: expression += "min(int(" + a + "), int(" + b + "))"; break;
                case DXILOP_UMax: expression += "max(uint(" + a + "), uint(" + b + "))"; break;
                case DXILOP_UMin: expression += "min(uint(" + a + "), uint(" + b + "))"; break;
                case DXILOP_IMul: expression += "int(" + a + ") * int(" + b + ")"; break;
                case DXILOP_UMul: expression += "uint(" + a + ") * uint(" + b + ")"; break;
                case DXILOP_UDiv: expression += "uint(" + a + ") / uint(" + b + ")"; break;
                case DXILOP_UAddc: expression += "uint(" + a + ") + uint(" + b + ")"; break;
                case DXILOP_USubb: expression += "uint(" + a + ") - uint(" + b + ")"; break;
                default: ctx.unsupported_intrinsics++; expression += "0"; break;
                }
            }
            expression += "}}";
            return expression;
        }
        auto a = numericArg(1, "0"), b = numericArg(2, "0");
        const bool double_op = lhs_type.kind == MSLTypeKind::Double ||
                               rhs_type.kind == MSLTypeKind::Double;
        auto da = double_op ? doubleArg(1, "0") : a;
        auto db = double_op ? doubleArg(2, "0") : b;
        switch (op) {
        case DXILOP_FMax:
            return double_op ? "(m12_f64_cmp(ulong(" + da + "), ulong(" + db + "), 3u) ? ulong(" + da + ") : ulong(" + db + "))" : "max(static_cast<float>(" + a + "), static_cast<float>(" + b + "))";
        case DXILOP_FMin:
            return double_op ? "(m12_f64_cmp(ulong(" + da + "), ulong(" + db + "), 4u) ? ulong(" + da + ") : ulong(" + db + "))" : "min(static_cast<float>(" + a + "), static_cast<float>(" + b + "))";
        case DXILOP_IMax: return "max(static_cast<int>(" + a + "), static_cast<int>(" + b + "))";
        case DXILOP_IMin: return "min(static_cast<int>(" + a + "), static_cast<int>(" + b + "))";
        case DXILOP_UMax: return "max((uint)(" + a + "), (uint)(" + b + "))";
        case DXILOP_UMin: return "min((uint)(" + a + "), (uint)(" + b + "))";
        case DXILOP_IMul: return "mul24(" + a + ", " + b + ")";
        case DXILOP_UMul: return "mul24((uint)(" + a + "), (uint)(" + b + "))";
        case DXILOP_UDiv: return "((uint)(" + a + ") / (uint)(" + b + "))";
        case DXILOP_UAddc: return "((" + a + ") + (" + b + "))";
        case DXILOP_USubb: return "((" + a + ") - (" + b + "))";
        default: ctx.unsupported_intrinsics++; return a;
        }
    }
    case DXOP_Tertiary: {
        if (args.size() < 4) return "0";
        uint32_t op = literalArg(0, 0xFFFFFFFFu, "tertiary");
        const MSLType a_type = valueTypeOrUnknown(ctx, args[1]);
        const MSLType b_type = valueTypeOrUnknown(ctx, args[2]);
        const MSLType c_type = valueTypeOrUnknown(ctx, args[3]);
        const MSLType vector_type = DXILIRBuilder::isLongVectorType(a_type)
                                        ? a_type
                                        : DXILIRBuilder::isLongVectorType(b_type)
                                              ? b_type
                                              : c_type;
        if (DXILIRBuilder::isLongVectorType(vector_type)) {
            const std::string av = DXILIRBuilder::isLongVectorType(a_type)
                                       ? vectorArg(1, "{}")
                                       : numericArg(1, "0");
            const std::string bv = DXILIRBuilder::isLongVectorType(b_type)
                                       ? vectorArg(2, "{}")
                                       : numericArg(2, "0");
            const std::string cv = DXILIRBuilder::isLongVectorType(c_type)
                                       ? vectorArg(3, "{}")
                                       : numericArg(3, "0");
            const bool a_vector = DXILIRBuilder::isLongVectorType(a_type) ||
                                  startsWith(av, "array<");
            const bool b_vector = DXILIRBuilder::isLongVectorType(b_type) ||
                                  startsWith(bv, "array<");
            const bool c_vector = DXILIRBuilder::isLongVectorType(c_type) ||
                                  startsWith(cv, "array<");
            MSLType result_type = vector_type;
            if (op == DXILOP_IMad || op == DXILOP_Ibfe || op == DXILOP_Bfi)
                result_type.vector_element_kind = MSLTypeKind::Int;
            else if (op == DXILOP_UMad || op == DXILOP_Ubfe)
                result_type.vector_element_kind = MSLTypeKind::UInt;
            std::string expression = emitTypeName(result_type) + "{{";
            for (uint32_t i = 0; i < result_type.vector_width; ++i) {
                if (i)
                    expression += ", ";
                const std::string a = a_vector
                                           ? av + "[" + std::to_string(i) + "]"
                                           : av;
                const std::string b = b_vector
                                           ? bv + "[" + std::to_string(i) + "]"
                                           : bv;
                const std::string c = c_vector
                                           ? cv + "[" + std::to_string(i) + "]"
                                           : cv;
                switch (op) {
                case DXILOP_FMad:
                case DXILOP_Fma: expression += "fma(" + a + ", " + b + ", " + c + ")"; break;
                case DXILOP_IMad: expression += "int(" + a + ") * int(" + b + ") + int(" + c + ")"; break;
                case DXILOP_UMad: expression += "uint(" + a + ") * uint(" + b + ") + uint(" + c + ")"; break;
                case DXILOP_Ibfe: expression += "extract_bits(int(" + a + "), uint(" + b + "), uint(" + c + "))"; break;
                case DXILOP_Ubfe: expression += "extract_bits(uint(" + a + "), uint(" + b + "), uint(" + c + "))"; break;
                case DXILOP_Bfi: expression += "insert_bits(uint(" + b + "), uint(" + a + "), uint(" + c + "))"; break;
                default: ctx.unsupported_intrinsics++; expression += "0"; break;
                }
            }
            expression += "}}";
            return expression;
        }
        auto a = numericArg(1, "0"), b = numericArg(2, "0"), c = numericArg(3, "0");
        const bool double_op = a_type.kind == MSLTypeKind::Double ||
                               b_type.kind == MSLTypeKind::Double ||
                               c_type.kind == MSLTypeKind::Double;
        auto da = double_op ? doubleArg(1, "0") : a;
        auto db = double_op ? doubleArg(2, "0") : b;
        auto dc = double_op ? doubleArg(3, "0") : c;
        switch (op) {
        case DXILOP_FMad: case DXILOP_Fma:
            return double_op ? "m12_f64_add(m12_f64_mul(ulong(" + da + "), ulong(" + db + ")), ulong(" + dc + "))" : "fma(static_cast<float>(" + a + "), static_cast<float>(" + b +
                   "), static_cast<float>(" + c + "))";
        case DXILOP_IMad: case DXILOP_UMad: return "((" + a + ") * (" + b + ") + (" + c + "))";
        case DXILOP_Msad: {
            const char *shifts[] = {"0u", "8u", "16u", "24u"};
            std::string result = "(uint(" + c + ")";
            for (const char *shift : shifts) {
                std::string ref = "((uint)(" + a + ") >> " + shift + ") & 0xffu";
                std::string src = "((uint)(" + b + ") >> " + shift + ") & 0xffu";
                result += " + (" + ref + " != 0u ? uint(abs(int(" + ref + ") - int(" + src + "))) : 0u)";
            }
            return result + ")";
        }
        case DXILOP_Ibfe: return "extract_bits(" + a + ", " + b + ", " + c + ")";
        case DXILOP_Ubfe: return "extract_bits((uint)(" + a + "), (uint)(" + b + "), (uint)(" + c + "))";
        case DXILOP_Bfi: return "insert_bits((uint)(" + b + "), (uint)(" + a + "), (uint)(" + c + "))";
        default: ctx.unsupported_intrinsics++; return a;
        }
    }
    case DXOP_Dot2: {
        if (args.size() < 4) return "0.0";
        const bool double_op = valueTypeOrUnknown(ctx, args[0]).kind == MSLTypeKind::Double ||
                               valueTypeOrUnknown(ctx, args[1]).kind == MSLTypeKind::Double ||
                               valueTypeOrUnknown(ctx, args[2]).kind == MSLTypeKind::Double ||
                               valueTypeOrUnknown(ctx, args[3]).kind == MSLTypeKind::Double;
        if (double_op)
            return "m12_f64_add(m12_f64_mul(ulong(" + doubleArg(0, "0") + "), ulong(" + doubleArg(2, "0") + ")), m12_f64_mul(ulong(" + doubleArg(1, "0") + "), ulong(" + doubleArg(3, "0") + ")))";
        return "((" + numericArg(0,"0.0") + ")*(" + numericArg(2,"0.0") + ") + (" + numericArg(1,"0.0") + ")*(" + numericArg(3,"0.0") + "))";
    }
    case DXOP_Dot3: {
        if (args.size() < 6) return "0.0";
        bool double_op = false;
        for (unsigned i = 0; i < 6; ++i)
            double_op = double_op || valueTypeOrUnknown(ctx, args[i]).kind == MSLTypeKind::Double;
        if (double_op) {
            std::string result = "m12_f64_mul(ulong(" + doubleArg(0, "0") + "), ulong(" + doubleArg(3, "0") + "))";
            for (unsigned i = 1; i < 3; ++i)
                result = "m12_f64_add(" + result + ", m12_f64_mul(ulong(" + doubleArg(i, "0") + "), ulong(" + doubleArg(i + 3, "0") + ")))";
            return result;
        }
        return "((" + numericArg(0,"0.0") + ")*(" + numericArg(3,"0.0") + ") + (" + numericArg(1,"0.0") + ")*(" + numericArg(4,"0.0") + ") + (" + numericArg(2,"0.0") + ")*(" + numericArg(5,"0.0") + "))";
    }
    case DXOP_FDot: {
        if (args.size() < 2) return "0.0f";
        const MSLType lhs_type = valueTypeOrUnknown(ctx, args[0]);
        const MSLType rhs_type = valueTypeOrUnknown(ctx, args[1]);
        const MSLType vector_type = DXILIRBuilder::isLongVectorType(lhs_type)
                                        ? lhs_type
                                        : rhs_type;
        const std::string lhs = vectorArg(0, "float2(0.0f)");
        const std::string rhs = vectorArg(1, "float2(0.0f)");
        if (DXILIRBuilder::isLongVectorType(vector_type)) {
            std::string result = "0.0f";
            for (uint32_t i = vector_type.vector_width; i-- > 0;) {
                result = "(" + lhs + "[" + std::to_string(i) + "] * " +
                         rhs + "[" + std::to_string(i) + "] + " + result + ")";
            }
            return result;
        }
        return "dot(" + lhs + ", " + rhs + ")";
    }
    case DXOP_Dot4: {
        if (args.size() < 8) return "0.0";
        bool double_op = false;
        for (unsigned i = 0; i < 8; ++i)
            double_op = double_op || valueTypeOrUnknown(ctx, args[i]).kind == MSLTypeKind::Double;
        if (double_op) {
            std::string result = "m12_f64_mul(ulong(" + doubleArg(0, "0") + "), ulong(" + doubleArg(4, "0") + "))";
            for (unsigned i = 1; i < 4; ++i)
                result = "m12_f64_add(" + result + ", m12_f64_mul(ulong(" + doubleArg(i, "0") + "), ulong(" + doubleArg(i + 4, "0") + ")))";
            return result;
        }
        return "((" + numericArg(0,"0.0") + ")*(" + numericArg(4,"0.0") + ") + (" + numericArg(1,"0.0") + ")*(" + numericArg(5,"0.0") + ") + (" + numericArg(2,"0.0") + ")*(" + numericArg(6,"0.0") + ") + (" + numericArg(3,"0.0") + ")*(" + numericArg(7,"0.0") + "))";
    }
    case DXOP_Dot2AddHalf:
        if (args.size() < 5) return "0.0";
        return "fma(float(" + numericArg(1, "0.0") + "), float(" + numericArg(3, "0.0") + "), fma(float(" +
               numericArg(2, "0.0") + "), float(" + numericArg(4, "0.0") + "), float(" +
               numericArg(0, "0.0") + ")))";
    case DXOP_Dot4AddI8Packed:
    case DXOP_Dot4AddU8Packed: {
        if (args.size() < 3) return "0";
        const bool signed_bytes = intrinsic_id == DXOP_Dot4AddI8Packed;
        auto byte = [&](const std::string &value, unsigned shift) {
            std::string bits = "((uint(" + value + ") >> " + std::to_string(shift) + "u) & 0xffu)";
            if (!signed_bytes)
                return bits;
            return "((int(" + bits + " << 24)) >> 24)";
        };
        std::string result = signed_bytes ? "int(" + numericArg(0, "0") + ")"
                                          : "uint(" + numericArg(0, "0") + ")";
        for (unsigned shift = 0; shift < 32; shift += 8)
            result += " + (" + byte(numericArg(1, "0"), shift) + " * " +
                      byte(numericArg(2, "0"), shift) + ")";
        return signed_bytes ? result : "uint(" + result + ")";
    }
    case DXOP_LoadInput: {
        if (args.size() < 3) return "0.0";
        uint32_t input_id = literalArg(0, 0, "input");
        uint32_t comp = literalArg(2, 0, "comp");
        if (ctx.shader.kind == DxilShaderKind::Pixel) {
            if (ctx.shader.shading_rate_input_register >= 0 &&
                static_cast<int32_t>(input_id) ==
                    ctx.shader.shading_rate_input_register)
                return ctx.options.vrs_per_primitive
                           ? "m12_vrs_state.y"
                           : "static_cast<uint>(" +
                                 varyingField("in", input_id) + ".x)";
            if (ctx.shader.viewport_index_input_register >= 0 &&
                static_cast<int32_t>(input_id) ==
                    ctx.shader.viewport_index_input_register)
                return "static_cast<uint>(in.viewport_array_index)";
            if (ctx.shader.render_target_array_index_input_register >= 0 &&
                static_cast<int32_t>(input_id) ==
                    ctx.shader.render_target_array_index_input_register)
                return "static_cast<uint>(in.render_target_array_index)";
            const std::string field = varyingField("in", input_id);
            if (ctx.uses_interpolation && input_id != 0)
                return field + ".interpolate_at_center()" + componentSuffix(comp);
            return field + componentSuffix(comp);
        }
        if (ctx.shader.kind == DxilShaderKind::Vertex) {
            if (isLoadInputI32(callee_name) && shouldLowerLoadInputI32AsVertexId(ctx, input_id))
                return comp == 0 ? "vid" : "0u";
            if (isLoadInputI32(callee_name))
                return "static_cast<uint>(" + vertexPullField(ctx, input_id) +
                       componentSuffix(comp) + ")";
            return vertexPullField(ctx, input_id) + componentSuffix(comp);
        }
        return "0.0";
    }
    case DXOP_StoreOutput: {
        if (args.size() < 4) return "";
        uint32_t output_id = literalArg(0, 0, "output");
        uint32_t comp = literalArg(2, 0, "comp");
        auto val = numericArg(3, "0.0");
        if (ctx.shader.kind == DxilShaderKind::Vertex &&
            ctx.vertex_procedural_fullscreen_fallback) {
            recordDiagnostic(ctx,
                             "storeOutput skipped procedural vertex default output=%u comp=%u value=%s",
                             output_id, comp, val.c_str());
            return "";
        }
        if (ctx.shader.kind == DxilShaderKind::Vertex) {
            if (ctx.shader.shading_rate_output_id >= 0 &&
                static_cast<int32_t>(output_id) ==
                    ctx.shader.shading_rate_output_id)
                return "out.shading_rate = static_cast<uint>(" + val + ")";
            if (ctx.shader.viewport_index_output_id >= 0 &&
                static_cast<int32_t>(output_id) ==
                    ctx.shader.viewport_index_output_id)
                return "out.viewport_array_index = static_cast<uint>(" + val + ")";
            if (ctx.shader.render_target_array_index_output_id >= 0 &&
                static_cast<int32_t>(output_id) ==
                    ctx.shader.render_target_array_index_output_id)
                return "out.render_target_array_index = static_cast<uint>(" + val + ")";
            bool simple_input_passthrough =
                ctx.current_fn && ctx.current_fn->name.find("SimpleVS") != std::string::npos;
            if (!ctx.options.vertex_inputs.empty() &&
                hasVertexInputForRegister(ctx, output_id) &&
                (valueIsUnresolvedTemp(ctx, args[3]) || simple_input_passthrough))
                val = vertexPullField(ctx, output_id) + componentSuffix(comp);
            return varyingField("out", output_id) + componentSuffix(comp) + " = " + val;
        }
        if (ctx.shader.kind == DxilShaderKind::Pixel) return std::string("result") + componentSuffix(comp) + " = " + val;
        return "";
    }
    case DXOP_MakeDouble:
        if (args.size() < 2) return "ulong(0)";
        return "(ulong(uint(" + numericArg(0, "0") + ")) | (ulong(uint(" +
               numericArg(1, "0") + ")) << 32))";
    case DXOP_SplitDouble: {
        auto bits = "ulong(" + numericArg(0, "0") + ")";
        return "uint2(uint(" + bits + "), uint(" + bits + " >> 32))";
    }
    case DXOP_BitcastI16ToF16:
        return "as_type<half>(short(" + numericArg(0, "0") + "))";
    case DXOP_BitcastF16ToI16:
        return "as_type<short>(half(" + numericArg(0, "0.0") + "))";
    case DXOP_BitcastI32ToF32:
        return "as_type<float>(uint(" + numericArg(0, "0") + "))";
    case DXOP_BitcastF32ToI32:
        return "as_type<int>(float(" + numericArg(0, "0.0") + "))";
    case DXOP_BitcastI64ToF64:
        return "ulong(" + numericArg(0, "0") + ")";
    case DXOP_BitcastF64ToI64:
        return "as_type<long>(ulong(" + numericArg(0, "0") + "))";
    case DXOP_LegacyF32ToF16:
        return "static_cast<uint>(as_type<ushort>(half(" + numericArg(0, "0.0") + ")))";
    case DXOP_LegacyF16ToF32:
        return "static_cast<float>(as_type<half>(ushort(" + numericArg(0, "0") + ")))";
    case DXOP_LegacyDoubleToFloat:
        return "m12_f64_to_float(ulong(" + doubleArg(0, "0") + "))";
    case DXOP_LegacyDoubleToSInt32:
        return "m12_f64_to_sint(ulong(" + doubleArg(0, "0") + "))";
    case DXOP_LegacyDoubleToUInt32:
        return "m12_f64_to_uint(ulong(" + doubleArg(0, "0") + "))";
    case DXOP_WaveReadLaneFirst: return "simd_broadcast_first(" + numericArg(0, "0") + ")";
    case DXOP_WaveReadLaneAt: return "simd_broadcast(" + numericArg(0, "0") + ", (uint)(" + numericArg(1, "0") + "))";
    case DXOP_WaveIsFirstLane: return "(simd_lane == 0u)";
    case DXOP_WaveGetLaneIndex: return "simd_lane";
    case DXOP_WaveGetLaneCount: return "simd_count";
    case DXOP_WaveAnyTrue: return "simd_any(" + numericArg(0, "0") + ") ? 1 : 0";
    case DXOP_WaveAllTrue: return "simd_all(" + numericArg(0, "0") + ") ? 1 : 0";
    case DXOP_WaveActiveAllEqual: {
        auto value = numericArg(0, "0");
        return "simd_all((" + value + ") == simd_broadcast_first(" + value + "))";
    }
    case DXOP_WaveActiveBallot:
        return "uint4(static_cast<uint>(static_cast<simd_vote::vote_t>(simd_ballot(" +
               numericArg(0, "0") + "))), 0u, 0u, 0u)";
    case DXOP_WaveAllBitCount:
        return "simd_sum(uint(" + numericArg(0, "0") + "))";
    case DXOP_WavePrefixBitCount:
        return "simd_prefix_exclusive_sum(uint(" + numericArg(0, "0") + "))";
    case DXOP_WaveMatch: {
        auto value = numericArg(0, "0");
        return "uint4(m12_wave_match(uint(" + value + "), simd_lane, simd_count), 0u, 0u, 0u)";
    }
    case DXOP_WaveMultiPrefixBitCount:
        return "m12_wave_multi_prefix_bit_count(uint(" + numericArg(0, "0") + "), uint(" +
               numericArg(1, "0") + "), simd_lane)";
    case DXOP_WaveMultiPrefixOp: {
        auto mask = "uint(" + numericArg(1, "0") + ")";
        uint32_t op = literalArg(5, 0xFFFFFFFFu, "wave_multi_prefix_op");
        if (op > 4u) {
            ctx.unsupported_intrinsics++;
            return numericArg(0, "0");
        }
        const std::string value = numericArg(0, "0");
        const std::string lane = "simd_lane";
        switch (op) {
        case 0u:
            return "m12_wave_multi_prefix_sum(" + value + ", " + mask + ", " + lane + ")";
        case 1u:
            return "m12_wave_multi_prefix_bit_and(" + value + ", " + mask + ", " + lane + ")";
        case 2u:
            return "m12_wave_multi_prefix_bit_or(" + value + ", " + mask + ", " + lane + ")";
        case 3u:
            return "m12_wave_multi_prefix_bit_xor(" + value + ", " + mask + ", " + lane + ")";
        case 4u:
            return "m12_wave_multi_prefix_product(" + value + ", " + mask + ", " + lane + ")";
        default:
            ctx.unsupported_intrinsics++;
            return value;
        }
    }
    case DXOP_WaveActiveOp: {
        auto value = numericArg(0, "0");
        uint32_t op = literalArg(1, 0xFFFFFFFFu, "wave_active_op");
        if (callee_name.find(".i64") != std::string::npos) {
            if (op == 0u)
                return "m12_wave_active_sum_long((long)(" + value + "))";
            if (op == 1u)
                return "m12_wave_active_product_long((long)(" + value + "))";
            if (op == 2u || op == 3u)
                return "m12_wave_active_extreme_long((long)(" + value + "), " +
                       std::to_string(literalArg(2, 0u, "wave_active_signed")) +
                       "u, " + (op == 2u ? "true" : "false") + ")";
            ctx.unsupported_intrinsics++;
            return value;
        }
        switch (op) {
        case 0: return "simd_sum(" + value + ")";
        case 1: return "simd_product(" + value + ")";
        case 2: return "simd_min(" + value + ")";
        case 3: return "simd_max(" + value + ")";
        default: ctx.unsupported_intrinsics++; return value;
        }
    }
    case DXOP_WaveActiveBit: {
        auto value = numericArg(0, "0");
        uint32_t op = literalArg(1, 0xFFFFFFFFu, "wave_active_bit");
        if (callee_name.find(".i64") != std::string::npos) {
            if (op <= 2u)
                return "m12_wave_active_bit_long((long)(" + value + "), " + std::to_string(op) + "u)";
            ctx.unsupported_intrinsics++;
            return value;
        }
        switch (op) {
        case 0: return "simd_and(" + value + ")";
        case 1: return "simd_or(" + value + ")";
        case 2: return "simd_xor(" + value + ")";
        default: ctx.unsupported_intrinsics++; return value;
        }
    }
    case DXOP_WavePrefixOp: {
        auto value = numericArg(0, "0");
        uint32_t op = literalArg(1, 0xFFFFFFFFu, "wave_prefix_op");
        if (callee_name.find(".i64") != std::string::npos) {
            if (op == 0u)
                return "m12_wave_prefix_sum_long((long)(" + value + "), simd_lane)";
            if (op == 1u)
                return "m12_wave_prefix_product_long((long)(" + value + "), simd_lane)";
            ctx.unsupported_intrinsics++;
            return value;
        }
        switch (op) {
        case 0: return "simd_prefix_exclusive_sum(" + value + ")";
        case 1: return "simd_prefix_exclusive_product(" + value + ")";
        default: ctx.unsupported_intrinsics++; return value;
        }
    }
    case DXOP_QuadReadLaneAt:
        return "quad_broadcast(" + numericArg(0, "0") + ", (uint)(" + numericArg(1, "0") + "))";
    case DXOP_QuadOp: {
        auto value = numericArg(0, "0");
        uint32_t op = literalArg(1, 0xFFFFFFFFu, "quad_op");
        switch (op) {
        case 0: return "quad_shuffle_xor(" + value + ", 1u)";
        case 1: return "quad_shuffle_xor(" + value + ", 2u)";
        case 2: return "quad_shuffle_xor(" + value + ", 3u)";
        default: ctx.unsupported_intrinsics++; return value;
        }
    }
    case DXOP_IsHelperLane:
        return "simd_is_helper_thread()";
    case DXOP_QuadVote: {
        uint32_t op = literalArg(1, 0xFFFFFFFFu, "quad_vote");
        if (op == 0)
            return "quad_any(" + numericArg(0, "0") + ")";
        if (op == 1)
            return "quad_all(" + numericArg(0, "0") + ")";
        ctx.unsupported_intrinsics++;
        return "false";
    }
    default:
        ctx.unsupported_intrinsics++;
        break;
    }
    return "0";
}

static void emitTypedInstruction(LowerContext &ctx, const LLVMInstruction &inst, uint32_t &value_counter) {
    auto &os = ctx.os;
    std::string result = emitValue(value_counter);

    auto getValue = [&](uint32_t idx) -> std::string {
        if (idx < ctx.value_table.size() && !ctx.value_table[idx].empty()) {
            const auto &v = ctx.value_table[idx];
            if (startsWith(v, "dx.")) {
                // function name, not a value — fall through to constants/emitValue
            } else if (startsWith(v, "agg(")) {
                MSLType type = idx < ctx.value_types.size() ? ctx.value_types[idx] : MSLType{};
                return aggregateConstructor(v, type);
            } else if (v.find('.') != std::string::npos) {
                return v;
            } else {
                return v;
            }
        }
        for (auto &c : ctx.mod.constants)
            if (c.id == idx && !c.constant_data.empty())
                return normalizeAggregateExpressions(c.constant_data, DXILIRBuilder::resolveType(c.type_id, ctx.mod));
        if (ctx.current_fn)
            for (auto &c : ctx.current_fn->constants)
                if (c.id == idx && !c.constant_data.empty())
                    return normalizeAggregateExpressions(c.constant_data, DXILIRBuilder::resolveType(c.type_id, ctx.mod));
        return emitValue(idx);
    };

    auto ensureValueTable = [&](uint32_t needed) {
        if (ctx.value_table.size() <= needed) ctx.value_table.resize(needed + 1);
        if (ctx.value_types.size() <= needed) ctx.value_types.resize(needed + 1);
        if (ctx.value_roles.size() <= needed) ctx.value_roles.resize(needed + 1);
    };

    auto getTypeForInst = [&](uint32_t type_id) -> MSLType {
        if (type_id < ctx.mod.types.size())
            return DXILIRBuilder::resolveType(type_id, ctx.mod);
        return {MSLTypeKind::Unknown, 0, {}};
    };

    auto inferTypeFromExpr = [](const std::string &expr) -> MSLType {
        if (startsWith(expr, "array<")) {
            const size_t comma = expr.find(',', 6);
            const size_t close = expr.find('>', comma == std::string::npos ? 6 : comma);
            if (comma != std::string::npos && close != std::string::npos) {
                MSLType result;
                result.kind = MSLTypeKind::LongVector;
                std::string element = expr.substr(6, comma - 6);
                result.vector_width = static_cast<uint32_t>(std::strtoul(
                    expr.substr(comma + 1, close - comma - 1).c_str(), nullptr, 10));
                result.vector_element_kind = element == "uint"
                                                  ? MSLTypeKind::UInt
                                                  : element == "int"
                                                        ? MSLTypeKind::Int
                                                        : MSLTypeKind::Float;
                return result;
            }
        }
        if (startsWith(expr, "buf"))
            return {MSLTypeKind::DeviceCharPtr, 0, {}};
        if (startsWith(expr, "tex"))
            return {MSLTypeKind::RWTexture2D, 0, {}};
        if (startsWith(expr, "samp"))
            return {MSLTypeKind::Sampler, 0, {}};
        if (exprLooksScalarMathCall(expr))
            return {MSLTypeKind::Float, 0, {}};
        if (expr.find(".calculate_clamped_lod(") != std::string::npos ||
            expr.find(".calculate_unclamped_lod(") != std::string::npos)
            return {MSLTypeKind::Float, 0, {}};
        if (expr.find("m12_dynamic_buffer_load_") != std::string::npos)
            return {MSLTypeKind::UInt4, 0, {}};
        if (expr.find("reinterpret_cast<device float4&>") != std::string::npos)
            return {MSLTypeKind::Float4, 0, {}};
        if (expr.find("reinterpret_cast<device uint4&>") != std::string::npos)
            return {MSLTypeKind::UInt4, 0, {}};
        if (expr.find("reinterpret_cast<device int4&>") != std::string::npos)
            return {MSLTypeKind::Int4, 0, {}};
        if (expr.find("reinterpret_cast<device float&>") != std::string::npos)
            return {MSLTypeKind::Float, 0, {}};
        if (expr.find("reinterpret_cast<device uint&>") != std::string::npos)
            return {MSLTypeKind::UInt, 0, {}};
        if (expr.find(".read(") != std::string::npos)
            return {MSLTypeKind::Float4, 0, {}};
        if (expr.find(".sample(") != std::string::npos)
            return {MSLTypeKind::Float4, 0, {}};
        if (expr.find(".gather(") != std::string::npos ||
            expr.find(".gather_compare(") != std::string::npos)
            return {MSLTypeKind::Float4, 0, {}};
        if (expr.find("float4(") == 0 || expr.find("(float4(") != std::string::npos)
            return {MSLTypeKind::Float4, 0, {}};
        if (expr.find("uint4(") == 0 || expr.find("(uint4(") != std::string::npos)
            return {MSLTypeKind::UInt4, 0, {}};
        if (expr.find("uint3(") == 0 || expr.find("(uint3(") != std::string::npos)
            return {MSLTypeKind::UInt3, 0, {}};
        if (expr.find("uint2(") == 0 || expr.find("(uint2(") != std::string::npos)
            return {MSLTypeKind::UInt2, 0, {}};
        if (expr.find("int4(") == 0 || expr.find("(int4(") != std::string::npos)
            return {MSLTypeKind::Int4, 0, {}};
        if (expr.find("int3(") == 0 || expr.find("(int3(") != std::string::npos)
            return {MSLTypeKind::Int3, 0, {}};
        if (expr.find("int2(") == 0 || expr.find("(int2(") != std::string::npos)
            return {MSLTypeKind::Int2, 0, {}};
        if (expr.find("float2(") == 0)
            return {MSLTypeKind::Float2, 0, {}};
        if (expr.find("float3(") == 0)
            return {MSLTypeKind::Float3, 0, {}};
        return {MSLTypeKind::Unknown, 0, {}};
    };

    auto inferLongVectorType = [&](uint32_t value_id,
                                   MSLTypeKind element_hint = MSLTypeKind::Unknown) -> MSLType {
        if (value_id < ctx.value_types.size() &&
            DXILIRBuilder::isLongVectorType(ctx.value_types[value_id]))
            return ctx.value_types[value_id];
        if (value_id < ctx.value_table.size()) {
            MSLType inferred = inferTypeFromExpr(ctx.value_table[value_id]);
            if (DXILIRBuilder::isLongVectorType(inferred))
                return inferred;
        }
        for (const auto &type : ctx.mod.types) {
            if (type.kind != LLVMType::Vector || type.bit_width <= 4 ||
                type.bit_width > 16)
                continue;
            MSLType result;
            result.kind = MSLTypeKind::LongVector;
            result.vector_width = type.bit_width;
            result.vector_element_kind = element_hint == MSLTypeKind::Unknown
                                              ? MSLTypeKind::Float
                                              : element_hint;
            if (!type.type_refs.empty()) {
                MSLType element = DXILIRBuilder::resolveType(type.type_refs[0], ctx.mod);
                if (element.kind == MSLTypeKind::Float ||
                    element.kind == MSLTypeKind::Half)
                    result.vector_element_kind = MSLTypeKind::Float;
                else if (element.kind == MSLTypeKind::UInt)
                    result.vector_element_kind = MSLTypeKind::UInt;
                else if (element.kind == MSLTypeKind::Int)
                    result.vector_element_kind = MSLTypeKind::Int;
            }
            return result;
        }
        return {};
    };

    auto bestType = [&](MSLType declared, const std::string &expr) -> MSLType {
        auto inferred = inferTypeFromExpr(expr);
        if (inferred.kind != MSLTypeKind::Unknown) return inferred;
        return declared;
    };

    auto emitTypedLine = [&](MSLType &type, const std::string &name, const std::string &expr) {
        auto scalarizeVectorSelectForScalar = [&](const std::string &value,
                                                  const MSLType &target) -> std::string {
            if (!(DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) ||
                DXILIRBuilder::isVectorType(target))
                return value;

            std::string then_branch;
            std::string else_branch;
            if (!splitTopLevelTernary(value, then_branch, else_branch))
                return value;

            MSLType then_type = typeForResolvedExpression(ctx, then_branch);
            MSLType else_type = typeForResolvedExpression(ctx, else_branch);
            if (!DXILIRBuilder::isVectorType(then_type) && exprLooksVectorValue(then_branch))
                then_type = inferTypeFromExpr(then_branch);
            if (!DXILIRBuilder::isVectorType(else_type) && exprLooksVectorValue(else_branch))
                else_type = inferTypeFromExpr(else_branch);
            if (DXILIRBuilder::isVectorType(then_type) || DXILIRBuilder::isVectorType(else_type) ||
                exprLooksVectorValue(then_branch) || exprLooksVectorValue(else_branch))
                return "(" + value + ").x";
            return value;
        };

        // AllocateRayQuery is represented by a real thread-local Metal query
        // object.  The DXIL value is an opaque token, so the normal
        // self-assignment path must not emit an assignment (intersection_query
        // deliberately has no copy assignment operator).
        if (type.kind == MSLTypeKind::RayQuery &&
            stripEnclosingParens(expr) == name) {
            if (ctx.predeclared_names.find(name) == ctx.predeclared_names.end())
                os << "  " << emitTypeName(type) << " " << name << " = {};\n";
            return;
        }

        std::string source_expr = stripEnclosingParens(expr) == name
            ? defaultForType(isUsableMSLType(type) ? type : MSLType{MSLTypeKind::Int, 0, {}})
            : expr;
        const bool is_sample_compare =
            source_expr.find(".sample_compare(") != std::string::npos;
        if (is_sample_compare)
            type = {MSLTypeKind::Float, 0, {}};
        uint32_t forward_source_id = 0;
        if (parseEmittedValueName(stripEnclosingParens(source_expr), forward_source_id) &&
            forward_source_id < ctx.value_table.size() &&
            ctx.value_table[forward_source_id].empty())
            source_expr = defaultForType(isUsableMSLType(type) ? type : MSLType{MSLTypeKind::Int, 0, {}});
        if (ctx.shader.kind != DxilShaderKind::Compute &&
            type.kind == MSLTypeKind::RWTexture2D)
            type = {MSLTypeKind::Texture2D, 0, {}};
        if (type.kind == MSLTypeKind::Unknown || type.kind == MSLTypeKind::Void ||
            type.kind == MSLTypeKind::Struct) {
            auto source_type = typeForResolvedExpression(ctx, source_expr);
            auto inferred = isUsableMSLType(source_type) ? source_type : inferTypeFromExpr(source_expr);
            if (inferred.kind != MSLTypeKind::Unknown) type = inferred;
            else if (exprContainsRawResourceHandle(source_expr)) type = {MSLTypeKind::Int, 0, {}};
        }
        if (ctx.predeclared_names.find(name) != ctx.predeclared_names.end()) {
            MSLType declared_type = type;
            auto pre_it = ctx.predeclared_types.find(name);
            if (pre_it != ctx.predeclared_types.end())
                declared_type = pre_it->second;
            if (is_sample_compare) {
                declared_type = {MSLTypeKind::Float, 0, {}};
                type = declared_type;
                ctx.predeclared_types[name] = declared_type;
            }
            if (is_sample_compare) {
                os << "  " << name << " = " << source_expr << ";\n";
                return;
            }
            if (ctx.shader.kind != DxilShaderKind::Compute &&
                declared_type.kind == MSLTypeKind::RWTexture2D)
                declared_type = {MSLTypeKind::Texture2D, 0, {}};
            std::string assigned = coerceResolvedValue(ctx, source_expr, declared_type);
            assigned = normalizeVectorConstructorArities(assigned);
            if (DXILIRBuilder::isVectorType(declared_type)) {
                MSLType assigned_type = inferTypeFromExpr(assigned);
                if (DXILIRBuilder::isVectorType(assigned_type) &&
                    assigned_type.kind != declared_type.kind)
                    assigned = coerceVectorWidth(assigned, assigned_type, declared_type);
                assigned = scalarizeNestedVectorConstructorArgs(assigned, declared_type);
            }
            uint32_t source_id = 0;
            if (assigned == source_expr && parseEmittedValueName(source_expr, source_id) &&
                source_id < ctx.value_types.size() &&
                DXILIRBuilder::isVectorType(ctx.value_types[source_id]) &&
                (DXILIRBuilder::isFloatType(declared_type) || DXILIRBuilder::isIntType(declared_type)) &&
                !DXILIRBuilder::isVectorType(declared_type)) {
                assigned = "(" + source_expr + ").x";
            }
            if (assigned == source_expr && DXILIRBuilder::isVectorType(type) &&
                (DXILIRBuilder::isFloatType(declared_type) || DXILIRBuilder::isIntType(declared_type)) &&
                !DXILIRBuilder::isVectorType(declared_type)) {
                std::string scalarized = scalarizeVectorOperands(ctx, source_expr);
                assigned = scalarized != source_expr ? scalarized :
                    (exprLooksVectorValue(source_expr) ? "(" + source_expr + ").x" : source_expr);
            }
            assigned = scalarizeVectorSelectForScalar(assigned, declared_type);
            assigned = dropInvalidScalarComponentAccess(ctx, assigned, declared_type);
            type = declared_type;
            os << "  " << name << " = " << assigned << ";\n";
            return;
        }
        if (is_sample_compare) {
            os << "  float " << name << " = " << source_expr << ";\n";
            return;
        }
        std::string assigned = coerceResolvedValue(ctx, source_expr, type);
        assigned = normalizeVectorConstructorArities(assigned);
        if (DXILIRBuilder::isVectorType(type)) {
            MSLType assigned_type = inferTypeFromExpr(assigned);
            if (DXILIRBuilder::isVectorType(assigned_type) &&
                assigned_type.kind != type.kind)
                assigned = coerceVectorWidth(assigned, assigned_type, type);
            assigned = scalarizeNestedVectorConstructorArgs(assigned, type);
        }
        assigned = scalarizeVectorSelectForScalar(assigned, type);
        assigned = dropInvalidScalarComponentAccess(ctx, assigned, type);
        if (type.kind != MSLTypeKind::Unknown && type.kind != MSLTypeKind::Void &&
            type.kind != MSLTypeKind::Struct)
            os << "  " << emitTypeName(type) << " " << name << " = " << assigned << ";\n";
        else
            os << "  auto " << name << " = " << assigned << ";\n";
    };

    auto promoteType = [](const MSLType &a, const MSLType &b) -> MSLType {
        if (DXILIRBuilder::isVectorType(a)) return a;
        if (DXILIRBuilder::isVectorType(b)) return b;
        if (a.kind == MSLTypeKind::Float || a.kind == MSLTypeKind::Double ||
            a.kind == MSLTypeKind::Half) return a;
        if (b.kind == MSLTypeKind::Float || b.kind == MSLTypeKind::Double ||
            b.kind == MSLTypeKind::Half) return b;
        return a;
    };

    auto isPointerType = [](const MSLType &t) -> bool {
        return t.kind == MSLTypeKind::DeviceCharPtr || t.kind == MSLTypeKind::ThreadgroupCharPtr;
    };

    auto isUsableType = [](const MSLType &t) -> bool {
        return t.kind != MSLTypeKind::Unknown && t.kind != MSLTypeKind::Void &&
               t.kind != MSLTypeKind::Struct;
    };

    auto valueType = [&](uint32_t idx) -> MSLType {
        if (idx < ctx.value_types.size()) return ctx.value_types[idx];
        return {};
    };

    auto operandType = [&](uint32_t idx) -> MSLType {
        MSLType tracked = valueType(idx);
        MSLType resolved = typeForResolvedExpression(ctx, getValue(idx));
        if (isUsableType(resolved) &&
            (typeLooksResourceHandle(resolved) || !isUsableType(tracked) ||
             DXILIRBuilder::isVectorType(resolved) ||
             DXILIRBuilder::isLongVectorType(resolved)))
            return resolved;
        MSLType inferred = inferTypeFromExpr(getValue(idx));
        if (isUsableType(inferred) &&
            (!isUsableType(tracked) || DXILIRBuilder::isVectorType(inferred) ||
             DXILIRBuilder::isLongVectorType(inferred)))
            return inferred;
        return tracked;
    };

    auto demotePointerType = [&](MSLType t, MSLTypeKind scalar_kind = MSLTypeKind::Int) -> MSLType {
        if (!isPointerType(t)) return t;
        return {scalar_kind, 0, {}};
    };

    auto integerTypeFor = [](MSLType t) -> MSLType {
        switch (t.kind) {
        case MSLTypeKind::Float2: return {MSLTypeKind::Int2, 0, {}};
        case MSLTypeKind::Float3: return {MSLTypeKind::Int3, 0, {}};
        case MSLTypeKind::Float4: return {MSLTypeKind::Int4, 0, {}};
        case MSLTypeKind::Float:
        case MSLTypeKind::Half:
        case MSLTypeKind::Double:
            return {MSLTypeKind::Int, 0, {} };
        case MSLTypeKind::LongVector: {
            MSLType result = t;
            result.vector_element_kind = t.vector_element_kind == MSLTypeKind::UInt
                                             ? MSLTypeKind::UInt
                                             : MSLTypeKind::Int;
            return result;
        }
        default:
            return t;
        }
    };

    auto castExpr = [&](const std::string &expr, const MSLType &target) -> std::string {
        std::string type_name = emitTypeName(target);
        if (type_name.empty() || type_name == "auto" || type_name == "void") return expr;
        if (target.kind == MSLTypeKind::Bool)
            return coerceResolvedValue(ctx, expr, target);
        if (DXILIRBuilder::isVectorType(target)) {
            MSLType source = typeForResolvedExpression(ctx, expr);
            if (!DXILIRBuilder::isVectorType(source))
                source = inferTypeFromExpr(expr);
            if (DXILIRBuilder::isVectorType(source) &&
                (DXILIRBuilder::vectorWidth(source) != DXILIRBuilder::vectorWidth(target) ||
                 DXILIRBuilder::scalarType(source).kind != DXILIRBuilder::scalarType(target).kind))
                return coerceVectorWidth(expr, source, target);
            MSLType scalar = DXILIRBuilder::scalarType(target);
            if (exprLooksScalarLiteral(stripEnclosingParens(expr)) &&
                DXILIRBuilder::isIntType(scalar)) {
                std::string scalar_name = emitTypeName(scalar);
                if (!scalar_name.empty() && scalar_name != "auto")
                    return type_name + "(static_cast<" + scalar_name + ">(" + expr + "))";
            }
            return type_name + "(" + expr + ")";
        }
        MSLType source = typeForResolvedExpression(ctx, expr);
        if (!DXILIRBuilder::isVectorType(source))
            source = inferTypeFromExpr(expr);
        if (DXILIRBuilder::isVectorType(source))
            return "static_cast<" + type_name + ">(" + componentAccess(expr, 0, source) + ")";
        return "static_cast<" + type_name + ">(" + expr + ")";
    };

    auto coerceOperand = [&](uint32_t idx, const MSLType &target) -> std::string {
        std::string value = getValue(idx);
        MSLType source = operandType(idx);
        uint32_t resolved_id = 0;
        if (!typeLooksResourceHandle(source) &&
            parseEmittedValueName(value, resolved_id) &&
            resolved_id < ctx.value_types.size())
            source = ctx.value_types[resolved_id];
        auto pre_it = ctx.predeclared_types.find(value);
        if (!typeLooksResourceHandle(source) && pre_it != ctx.predeclared_types.end())
            source = pre_it->second;
        if (target.kind == MSLTypeKind::Double &&
            source.kind == MSLTypeKind::Double &&
            !parseEmittedValueName(value, resolved_id))
            return doubleLiteralExpression(value);
        if ((target.kind == MSLTypeKind::DeviceCharPtr ||
             target.kind == MSLTypeKind::ThreadgroupCharPtr) &&
            (startsWith(value, "tex") || startsWith(value, "samp")))
            return defaultForType(target);
        if ((target.kind == MSLTypeKind::Texture2D ||
             target.kind == MSLTypeKind::Texture2DArray ||
             target.kind == MSLTypeKind::Texture3D ||
             target.kind == MSLTypeKind::TextureCube ||
             target.kind == MSLTypeKind::Texture2DMS ||
             target.kind == MSLTypeKind::RWTexture2D ||
             target.kind == MSLTypeKind::RWTexture2DArray ||
             target.kind == MSLTypeKind::RWTexture3D) &&
            !startsWith(value, "tex"))
            return defaultForType(target);
        if (target.kind == MSLTypeKind::InstanceAccelerationStructure &&
            !startsWith(value, "as"))
            return defaultForType(target);
        if (target.kind == MSLTypeKind::Sampler && !startsWith(value, "samp"))
            return defaultForType(target);
        if ((exprLooksResourceHandle(value) || exprContainsRawResourceHandle(value) ||
             typeLooksResourceHandle(source)) &&
            (DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)))
            return defaultForType(target);
        if (source.kind == MSLTypeKind::Bool &&
            (target.kind == MSLTypeKind::Int || target.kind == MSLTypeKind::UInt))
            return castExpr(value, target);
        if ((target.kind == MSLTypeKind::Int || target.kind == MSLTypeKind::UInt) &&
            exprLooksScalarMathCall(value))
            return castExpr(value, target);
        if (!isUsableType(target) || target.kind == source.kind)
            return value;

        if (isPointerType(source)) {
            if (DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target))
                return defaultForType(target);
            return value;
        }

        if (exprLooksThreadVector(value) &&
            (DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) &&
            !DXILIRBuilder::isVectorType(target))
            return castExpr(value + ".x", target);

        if (source.kind == MSLTypeKind::Unknown &&
            (DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)))
            return castExpr(value, target);
        if (!DXILIRBuilder::isVectorType(source) && DXILIRBuilder::isVectorType(target) &&
            ((DXILIRBuilder::isIntType(source) && DXILIRBuilder::isIntType(target)) ||
             (DXILIRBuilder::isFloatType(source) && DXILIRBuilder::isFloatType(target)) ||
             (DXILIRBuilder::isIntType(source) && DXILIRBuilder::isFloatType(target)) ||
             (DXILIRBuilder::isFloatType(source) && DXILIRBuilder::isIntType(target))))
            return castExpr(value, target);
        if (DXILIRBuilder::isIntType(source) && DXILIRBuilder::isFloatType(target))
            return castExpr(value, target);
        if (DXILIRBuilder::isFloatType(source) && DXILIRBuilder::isIntType(target))
            return castExpr(value, target);
        if (DXILIRBuilder::isVectorType(source) &&
            (DXILIRBuilder::isFloatType(target) || DXILIRBuilder::isIntType(target)) &&
            !DXILIRBuilder::isVectorType(target))
            return castExpr("(" + value + ").x", target);
        if (DXILIRBuilder::isVectorType(source) && DXILIRBuilder::isVectorType(target) &&
            (DXILIRBuilder::vectorWidth(source) != DXILIRBuilder::vectorWidth(target) ||
             DXILIRBuilder::scalarType(source).kind != DXILIRBuilder::scalarType(target).kind))
            return coerceVectorWidth(value, source, target);
        return value;
    };

    auto coerceShiftOperand = [&](uint32_t idx, const MSLType &target) -> std::string {
        std::string raw_value = getValue(idx);
        MSLType raw_source = operandType(idx);
        uint32_t resolved_id = 0;
        if (!typeLooksResourceHandle(raw_source) &&
            parseEmittedValueName(raw_value, resolved_id) &&
            resolved_id < ctx.value_types.size())
            raw_source = ctx.value_types[resolved_id];
        auto pre_it = ctx.predeclared_types.find(raw_value);
        if (!typeLooksResourceHandle(raw_source) && pre_it != ctx.predeclared_types.end())
            raw_source = pre_it->second;

        if (DXILIRBuilder::isVectorType(target)) {
            if (DXILIRBuilder::isVectorType(raw_source)) {
                if (DXILIRBuilder::vectorWidth(raw_source) != DXILIRBuilder::vectorWidth(target) ||
                    DXILIRBuilder::scalarType(raw_source).kind != DXILIRBuilder::scalarType(target).kind)
                    return coerceVectorWidth(raw_value, raw_source, target);
                return castExpr(raw_value, target);
            }
            MSLType raw_inferred = inferTypeFromExpr(raw_value);
            if (DXILIRBuilder::isVectorType(raw_inferred)) {
                if (DXILIRBuilder::vectorWidth(raw_inferred) != DXILIRBuilder::vectorWidth(target) ||
                    DXILIRBuilder::scalarType(raw_inferred).kind != DXILIRBuilder::scalarType(target).kind)
                    return coerceVectorWidth(raw_value, raw_inferred, target);
                return castExpr(raw_value, target);
            }
            if (exprLooksVectorValue(raw_value))
                return castExpr(raw_value, target);
        }

        std::string value = coerceOperand(idx, target);
        if (!DXILIRBuilder::isVectorType(target))
            return value;
        MSLType inferred = inferTypeFromExpr(value);
        if (DXILIRBuilder::isVectorType(inferred))
            return value;
        std::string type_name = emitTypeName(target);
        if (type_name.empty() || type_name == "auto")
            return value;
        MSLType scalar = DXILIRBuilder::scalarType(target);
        std::string scalar_name = emitTypeName(scalar);
        if (!scalar_name.empty() && scalar_name != "auto")
            return type_name + "(static_cast<" + scalar_name + ">(" + value + "))";
        return type_name + "(" + value + ")";
    };

    auto chooseBinaryType = [&](const MSLType &declared, const MSLType &lhs,
                                const MSLType &rhs, MSLTypeKind pointer_scalar) -> MSLType {
        MSLType result_type = demotePointerType(declared, pointer_scalar);
        if (typeLooksResourceHandle(result_type))
            result_type = {pointer_scalar, 0, {}};
        MSLType op0 = typeLooksResourceHandle(lhs) ? MSLType{pointer_scalar, 0, {}}
                                                   : demotePointerType(lhs, pointer_scalar);
        MSLType op1 = typeLooksResourceHandle(rhs) ? MSLType{pointer_scalar, 0, {}}
                                                   : demotePointerType(rhs, pointer_scalar);

        if (DXILIRBuilder::isVectorType(op0) ||
            DXILIRBuilder::isLongVectorType(op0))
            result_type = op0;
        else if (DXILIRBuilder::isVectorType(op1) ||
                 DXILIRBuilder::isLongVectorType(op1))
            result_type = op1;
        else if (op0.kind == MSLTypeKind::Double ||
                 op1.kind == MSLTypeKind::Double)
            result_type = {MSLTypeKind::Double, 0, {}};
        else if (DXILIRBuilder::isFloatType(op0) || DXILIRBuilder::isFloatType(op1))
            result_type = {MSLTypeKind::Float, 0, {}};
        else if (isUsableType(op0)) result_type = op0;
        else if (isUsableType(op1)) result_type = op1;

        if (!isUsableType(result_type)) result_type = {pointer_scalar, 0, {}};
        return result_type;
    };

    auto pointerAddressSpace = [&](uint32_t idx) -> const char * {
        MSLType type = valueType(idx);
        if (type.kind == MSLTypeKind::ThreadgroupCharPtr)
            return "threadgroup";
        std::string value = getValue(idx);
        if (startsWith(value, "(threadgroup") || startsWith(value, "threadgroup"))
            return "threadgroup";
        if (startsWith(value, "(thread") || startsWith(value, "thread"))
            return "thread";
        return "device";
    };

    switch (inst.opcode) {
    case LLVMInstruction::Ret:
        if (ctx.shader.kind == DxilShaderKind::Vertex) os << "  return out;\n";
        else if (ctx.shader.kind == DxilShaderKind::Pixel) os << "  return result;\n";
        else os << "  return;\n";
        break;

    case LLVMInstruction::Call: {
        if (inst.operands.empty()) break;
        bool call_produces_value = true;
        uint32_t callee = inst.operands[0];
        std::vector<uint32_t> call_args;
        for (size_t i = 2; i < inst.operands.size(); i++) call_args.push_back(inst.operands[i]);

        std::string callee_name;
        auto decl_it = ctx.function_decls.find(callee);
        if (decl_it != ctx.function_decls.end()) callee_name = decl_it->second;
        else if (callee < ctx.value_table.size()) callee_name = ctx.value_table[callee];
        uint32_t intrinsic_id = intrinsicIdFromCalleeName(callee_name);
        if (callee_name.find("dx.op.writeSamplerFeedbackLevel") !=
            std::string::npos)
            intrinsic_id = DXOP_WriteSamplerFeedbackLevel;
        else if (callee_name.find("dx.op.writeSamplerFeedbackGrad") !=
                 std::string::npos)
            intrinsic_id = DXOP_WriteSamplerFeedbackGrad;
        else if (callee_name.find("dx.op.writeSamplerFeedbackBias") !=
                 std::string::npos)
            intrinsic_id = DXOP_WriteSamplerFeedbackBias;
        else if (callee_name.find("dx.op.writeSamplerFeedback") !=
                 std::string::npos)
            intrinsic_id = DXOP_WriteSamplerFeedback;
        else if (callee_name.find("dx.op.textureGatherCmp") !=
                 std::string::npos)
            intrinsic_id = DXOP_TextureGatherCmp;
        bool opcode_prefixed_intrinsic = false;
        if (!call_args.empty() && (callee_name.empty() || startsWith(callee_name, "dx.op."))) {
            uint32_t opcode = literalFromValue(ctx, call_args[0], 0);
            if (isOpcodePrefixedDXIntrinsic(opcode)) {
                intrinsic_id = canonicalDXIntrinsicId(opcode);
                opcode_prefixed_intrinsic = true;
            }
        }
        if (intrinsic_id == DXOP_SpecialFloat && !call_args.empty()) {
            uint32_t opcode = literalFromValue(ctx, call_args[0], 0);
            if (opcode >= 8 && opcode <= 11)
                intrinsic_id = opcode;
            else
                intrinsic_id = 0;
        }

        const bool no_arg_start_location =
            call_args.empty() &&
            (intrinsic_id == DXOP_StartVertexLocation ||
             intrinsic_id == DXOP_StartInstanceLocation ||
             intrinsic_id == DXOP_ViewID);
        if (intrinsic_id != 0 && call_args.empty() &&
            !no_arg_start_location) {
            ensureValueTable(value_counter);
            if (intrinsic_id == DXOP_LoadInput && isLoadInputI32(callee_name) &&
                shouldLowerArgumentlessLoadInputI32AsVertexId(ctx)) {
                MSLType result_type = {MSLTypeKind::UInt, 0, {}};
                emitTypedLine(result_type, result, "vid");
                ctx.value_table[value_counter] = result;
                ctx.value_types[value_counter] = result_type;
            } else {
                ctx.unsupported_intrinsics++;
                ctx.value_table[value_counter] = result;
                ctx.value_types[value_counter] = getTypeForInst(inst.type_id);
            }
        } else if (intrinsic_id != 0) {
            std::vector<uint32_t> fn_args;
            if (no_arg_start_location)
                fn_args.clear();
            else if (opcode_prefixed_intrinsic)
                fn_args.assign(call_args.begin() + 1, call_args.end());
            else if (intrinsic_id == 13 || intrinsic_id == 14 || intrinsic_id == 15)
                fn_args = call_args;
            else
                fn_args.assign(call_args.begin() + 1, call_args.end());

            ctx.current_result_id = value_counter;
            std::string translated = translateDXIntrinsic(ctx, intrinsic_id, fn_args, callee_name);
            ctx.current_result_id = UINT32_MAX;
            MSLType result_type = inferDXIntrinsicResultType(
                ctx, intrinsic_id, fn_args, bestType(getTypeForInst(inst.type_id), translated),
                callee_name);
            if (translated.find(".sample_compare(") != std::string::npos)
                result_type = {MSLTypeKind::Float, 0, {}};
            ensureValueTable(value_counter);
            ctx.value_types[value_counter] = result_type;

            const bool atomic64_call =
                (intrinsic_id == DXOP_AtomicBinOp ||
                 intrinsic_id == DXOP_AtomicCompareExchange) &&
                callee_name.find(".i64") != std::string::npos;
            if (atomic64_call) {
                ctx.pending_handle.reset();
                if (ctx.predeclared_names.find(result) !=
                    ctx.predeclared_names.end())
                    os << "  " << result << " = " << translated << ";\n";
                else
                    os << "  " << emitTypeName(result_type) << " " << result
                       << " = " << translated << ";\n";
                ctx.value_table[value_counter] = result;
                ctx.value_types[value_counter] = result_type;
            } else if (ctx.pending_handle.has_value()) {
                ResourceHandleRecord handle = *ctx.pending_handle;
                auto planned_handle = ctx.resource_handles.find(value_counter);
                if (planned_handle != ctx.resource_handles.end() &&
                    !planned_handle->second.dynamic_index.empty() &&
                    !handle.dynamic_index.empty()) {
                    handle.lower_bound = planned_handle->second.lower_bound;
                    handle.binding_count =
                        planned_handle->second.binding_count;
                    handle.direct_heap = planned_handle->second.direct_heap;
                }
                ctx.resource_handles[value_counter] = handle;
                ctx.value_table[value_counter] = materializeHandleName(ctx, handle);
                ctx.value_types[value_counter] = typeForResourceHandle(ctx, handle);
                ctx.value_roles[value_counter] = roleForHandleKind(handle.kind);
                ctx.pending_handle.reset();
            } else if (result_type.kind == MSLTypeKind::Void || exprLooksSideEffectOnly(translated)) {
                call_produces_value = false;
                if (!translated.empty())
                    os << "  " << translated << ";\n";
            } else if (translated.empty()) {
                MSLType fallback_type = result_type;
                if (!isUsableType(fallback_type))
                    fallback_type = getTypeForInst(inst.type_id);
                if (!isUsableType(fallback_type))
                    fallback_type = {MSLTypeKind::Int, 0, {}};
                if (ctx.predeclared_names.find(result) != ctx.predeclared_names.end()) {
                    os << "  " << result << " = " << defaultForType(fallback_type) << ";\n";
                } else {
                    os << "  " << typedDecl(result, fallback_type) << " = "
                       << defaultForType(fallback_type) << ";\n";
                }
                ctx.value_table[value_counter] = result;
                ctx.value_types[value_counter] = fallback_type;
            } else if (!exprContainsAssignment(translated)) {
                if (!translated.empty() && translated[0] != ' ') {
                    const bool bare_handle =
                        translated.find('.') == std::string::npos &&
                        translated.find('(') == std::string::npos;
                    bool is_resource_handle =
                        bare_handle && (startsWith(translated, "buf") ||
                                        startsWith(translated, "tex") ||
                                        startsWith(translated, "samp"));
                    if (is_resource_handle) {
                        ctx.value_table[value_counter] = translated;
                    } else {
                        emitTypedLine(result_type, result, translated);
                        ctx.value_table[value_counter] = result;
                        ctx.value_types[value_counter] = result_type;
                    }
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
                ctx.value_table[value_counter] = translated;
            }
        } else if (callee_name == "llvm.lifetime.start" ||
                   callee_name == "llvm.lifetime.end") {
            // Lifetime intrinsics only constrain LLVM's optimizer; they do
            // not represent shader-visible work and are safe to omit.
            call_produces_value = false;
        } else {
            // A non-DXIL call would otherwise become a zero-valued temporary
            // and allow an unlowered helper/unknown intrinsic to reach MSL.
            // User-defined helper functions are not emitted by this backend,
            // so reject the shader until a real call graph lowering exists.
            ctx.unsupported_intrinsics++;
            recordDiagnostic(ctx, "DXIL call has no lowering: %s",
                             callee_name.empty() ? "<unknown>" : callee_name.c_str());
            ensureValueTable(value_counter);
            MSLType result_type = getTypeForInst(inst.type_id);
            if (!isUsableType(result_type))
                result_type = {MSLTypeKind::Int, 0, {}};
            if (result_type.kind != MSLTypeKind::Void) {
                if (ctx.predeclared_names.find(result) != ctx.predeclared_names.end())
                    os << "  " << result << " = " << defaultForType(result_type) << "; // call " << (callee_name.empty() ? getValue(callee) : callee_name) << "(";
                else
                    os << "  " << typedDecl(result, result_type) << " = 0; // call " << (callee_name.empty() ? getValue(callee) : callee_name) << "(";
            } else {
                os << "  // call " << (callee_name.empty() ? getValue(callee) : callee_name) << "(";
                call_produces_value = false;
            }
            for (size_t i = 0; i < call_args.size(); i++) {
                if (i) os << ", ";
                os << getValue(call_args[i]);
            }
            os << ")\n";
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
        }
        if (call_produces_value)
            value_counter++;
        break;
    }

    case LLVMInstruction::Add: case LLVMInstruction::Sub: case LLVMInstruction::Mul:
    case LLVMInstruction::UDiv: case LLVMInstruction::SDiv:
    case LLVMInstruction::URem: case LLVMInstruction::SRem:
    case LLVMInstruction::And: case LLVMInstruction::Or: case LLVMInstruction::Xor:
    case LLVMInstruction::Shl: case LLVMInstruction::LShr: case LLVMInstruction::AShr: {
        ensureValueTable(value_counter);
        const char *op_str = "+";
        switch (inst.opcode) {
        case LLVMInstruction::Add: op_str = "+"; break;
        case LLVMInstruction::Sub: op_str = "-"; break;
        case LLVMInstruction::Mul: op_str = "*"; break;
        case LLVMInstruction::UDiv: case LLVMInstruction::SDiv: op_str = "/"; break;
        case LLVMInstruction::URem: case LLVMInstruction::SRem: op_str = "%"; break;
        case LLVMInstruction::And: op_str = "&"; break;
        case LLVMInstruction::Or: op_str = "|"; break;
        case LLVMInstruction::Xor: op_str = "^"; break;
        case LLVMInstruction::Shl: op_str = "<<"; break;
        case LLVMInstruction::LShr: case LLVMInstruction::AShr: op_str = ">>"; break;
        default: break;
        }
        MSLType result_type = getTypeForInst(inst.type_id);
        if (inst.operands.size() >= 2)
            result_type = chooseBinaryType(result_type, operandType(inst.operands[0]),
                                           operandType(inst.operands[1]), MSLTypeKind::Int);
        if (inst.operands.size() >= 2 &&
            (operandType(inst.operands[0]).kind == MSLTypeKind::Long ||
             operandType(inst.operands[1]).kind == MSLTypeKind::Long))
            result_type = {MSLTypeKind::Long, 0, {}};
        auto pre_it = ctx.predeclared_types.find(result);
        if (pre_it != ctx.predeclared_types.end() &&
            (DXILIRBuilder::isVectorType(pre_it->second) ||
             DXILIRBuilder::isLongVectorType(pre_it->second)) &&
            (DXILIRBuilder::isFloatType(pre_it->second) || DXILIRBuilder::isIntType(pre_it->second)))
            result_type = pre_it->second;
        bool arithmetic_op = inst.opcode == LLVMInstruction::Add ||
                             inst.opcode == LLVMInstruction::Sub ||
                             inst.opcode == LLVMInstruction::Mul ||
                             inst.opcode == LLVMInstruction::UDiv ||
                             inst.opcode == LLVMInstruction::SDiv ||
                             inst.opcode == LLVMInstruction::URem ||
                             inst.opcode == LLVMInstruction::SRem;
        bool preserve_float_arithmetic = arithmetic_op && DXILIRBuilder::isFloatType(result_type);
        if (!preserve_float_arithmetic)
            result_type = integerTypeFor(result_type);
        if ((inst.opcode == LLVMInstruction::Shl || inst.opcode == LLVMInstruction::LShr ||
             inst.opcode == LLVMInstruction::AShr) &&
            !DXILIRBuilder::isVectorType(result_type) &&
            !DXILIRBuilder::isLongVectorType(result_type) &&
            result_type.kind != MSLTypeKind::Long)
            result_type = {MSLTypeKind::Int, 0, {}};
        bool is_shift = inst.opcode == LLVMInstruction::Shl ||
                        inst.opcode == LLVMInstruction::LShr ||
                        inst.opcode == LLVMInstruction::AShr;
        std::string lhs = is_shift ? coerceShiftOperand(inst.operands[0], result_type)
                                   : coerceOperand(inst.operands[0], result_type);
        std::string rhs = is_shift ? coerceShiftOperand(inst.operands[1], result_type)
                                   : coerceOperand(inst.operands[1], result_type);
        if (preserve_float_arithmetic &&
            (inst.opcode == LLVMInstruction::URem ||
             inst.opcode == LLVMInstruction::SRem) &&
            !DXILIRBuilder::isVectorType(result_type)) {
            lhs = castExpr(lhs, {MSLTypeKind::Float, 0, {}});
            rhs = castExpr(rhs, {MSLTypeKind::Float, 0, {}});
        }
        std::string expr;
        if (DXILIRBuilder::isLongVectorType(result_type)) {
            const MSLType lhs_type = operandType(inst.operands[0]);
            const MSLType rhs_type = operandType(inst.operands[1]);
            auto component = [&](const std::string &value, const MSLType &type,
                                 uint32_t index) {
                if (DXILIRBuilder::isLongVectorType(type))
                    return value + "[" + std::to_string(index) + "]";
                if (DXILIRBuilder::isVectorType(type))
                    return componentAccess(value, index, type);
                return value;
            };
            const std::string type_name = emitTypeName(result_type);
            expr = type_name + "{{";
            for (uint32_t i = 0; i < result_type.vector_width; ++i) {
                if (i)
                    expr += ", ";
                const std::string left = component(lhs, lhs_type, i);
                const std::string right = component(rhs, rhs_type, i);
                if (preserve_float_arithmetic &&
                    (inst.opcode == LLVMInstruction::URem ||
                     inst.opcode == LLVMInstruction::SRem))
                    expr += "fmod(" + left + ", " + right + ")";
                else
                    expr += left + " " + std::string(op_str) + " " + right;
            }
            expr += "}}";
        } else if (result_type.kind == MSLTypeKind::Double) {
            if (inst.opcode == LLVMInstruction::Add ||
                inst.opcode == LLVMInstruction::Sub) {
                if (inst.opcode == LLVMInstruction::Sub)
                    rhs = "(ulong(" + rhs + ") ^ (1ul << 63))";
                expr = "m12_f64_add(ulong(" + lhs + "), ulong(" + rhs + "))";
            } else if (inst.opcode == LLVMInstruction::Mul) {
                expr = "m12_f64_mul(ulong(" + lhs + "), ulong(" + rhs + "))";
            } else if (inst.opcode == LLVMInstruction::UDiv ||
                       inst.opcode == LLVMInstruction::SDiv) {
                expr = "m12_f64_div(ulong(" + lhs + "), ulong(" + rhs + "))";
            } else if (inst.opcode == LLVMInstruction::URem ||
                       inst.opcode == LLVMInstruction::SRem) {
                expr = "m12_f64_remainder(ulong(" + lhs + "), ulong(" + rhs + "))";
            } else {
                ctx.unsupported_opcodes++;
                recordDiagnostic(ctx, "unsupported binary64 integer opcode");
                expr = "ulong(0)";
            }
        } else {
            expr = preserve_float_arithmetic &&
                           (inst.opcode == LLVMInstruction::URem ||
                            inst.opcode == LLVMInstruction::SRem)
                ? "fmod(" + lhs + ", " + rhs + ")"
                : lhs + " " + std::string(op_str) + " " + rhs;
        }
        emitTypedLine(result_type, result, expr);
        ctx.value_table[value_counter] = result;
        ctx.value_types[value_counter] = result_type;
        value_counter++;
        break;
    }

    case LLVMInstruction::ExtractValue: {
        ensureValueTable(value_counter);
        MSLType result_type = getTypeForInst(inst.type_id);
        if (inst.operands.size() >= 2) {
            auto agg = getValue(inst.operands[0]);
            uint32_t idx = inst.operands[1];

            MSLType agg_type = {MSLTypeKind::Unknown, 0, {}};
            if (inst.operands[0] < ctx.value_types.size())
                agg_type = ctx.value_types[inst.operands[0]];

            bool is_struct = (agg_type.kind == MSLTypeKind::Struct);
            bool agg_is_vector = DXILIRBuilder::isVectorType(agg_type) ||
                                 DXILIRBuilder::isLongVectorType(agg_type);

            if (is_struct) {
                if (idx == 0) {
                    emitTypedLine(result_type, result, agg);
                } else {
                    emitTypedLine(result_type, result, defaultForType(result_type));
                }
            } else if (agg_is_vector && idx < DXILIRBuilder::vectorWidth(agg_type)) {
                std::string expr = DXILIRBuilder::isLongVectorType(agg_type)
                                       ? agg + "[" + std::to_string(idx) + "]"
                                       : componentAccess(agg, idx, agg_type);
                auto scalar = DXILIRBuilder::scalarType(agg_type);
                emitTypedLine(scalar, result, expr);
                result_type = scalar;
                ctx.vector_extract_origin[value_counter] = inst.operands[0];
            } else if (agg_is_vector == false && agg_type.kind != MSLTypeKind::Unknown) {
                emitTypedLine(result_type, result, agg);
            } else {
                auto inferred = inferTypeFromExpr(agg);
                if ((DXILIRBuilder::isVectorType(inferred) ||
                     DXILIRBuilder::isLongVectorType(inferred)) &&
                    idx < DXILIRBuilder::vectorWidth(inferred)) {
                    std::string expr = DXILIRBuilder::isLongVectorType(inferred)
                                           ? agg + "[" + std::to_string(idx) + "]"
                                           : componentAccess(agg, idx, inferred);
                    auto scalar = DXILIRBuilder::scalarType(inferred);
                    emitTypedLine(scalar, result, expr);
                    result_type = scalar;
                } else {
                    emitTypedLine(result_type, result, agg);
                }
            }
        } else {
            emitTypedLine(result_type, result, "0");
        }
        ctx.value_table[value_counter] = result;
        ctx.value_types[value_counter] = result_type;
        value_counter++;
        break;
    }

    case LLVMInstruction::InsertValue: {
        ensureValueTable(value_counter);
        MSLType result_type = getTypeForInst(inst.type_id);
        if (inst.operands.size() >= 1 && inst.operands[0] < ctx.value_types.size()) {
            auto &op_type = ctx.value_types[inst.operands[0]];
            if (op_type.kind != MSLTypeKind::Unknown && op_type.kind != MSLTypeKind::Struct)
                result_type = op_type;
        }
        auto agg = inst.operands.size() >= 1 ? getValue(inst.operands[0]) : "float4(0)";
        emitTypedLine(result_type, result, agg);
        if (inst.operands.size() >= 3 && inst.operands[2] < 4) {
            os << "  " << result << componentSuffix(inst.operands[2]) << " = " << getValue(inst.operands[1]) << ";\n";
        }
        ctx.value_table[value_counter] = result;
        ctx.value_types[value_counter] = result_type;
        value_counter++;
        break;
    }

    case LLVMInstruction::ExtractElement: {
        ensureValueTable(value_counter);
        MSLType result_type = getTypeForInst(inst.type_id);
        if (inst.operands.size() >= 2) {
            auto vec = getValue(inst.operands[0]);
            auto idx = getValue(inst.operands[1]);
            MSLType vec_type = inst.operands[0] < ctx.value_types.size() ? ctx.value_types[inst.operands[0]] : MSLType{};
            uint32_t idx_val = 0;
            std::string expr;
            if (!DXILIRBuilder::isVectorType(vec_type))
                expr = vec;
            else if (parseUnsignedLiteral(idx, idx_val) && idx_val < 4)
                expr = componentAccess(vec, idx_val, vec_type);
            else
                expr = vec + "[" + idx + "]";
            auto scalar = result_type.kind == MSLTypeKind::Unknown
                ? DXILIRBuilder::scalarType(vec_type)
                : result_type;
            // LLVM's signless i32 element type is commonly resolved as uint;
            // the long-vector representation retains the DXIL lane kind and
            // must win when extracting a signed lane for a later conversion.
            if (DXILIRBuilder::isLongVectorType(vec_type))
                scalar = DXILIRBuilder::scalarType(vec_type);
            emitTypedLine(scalar, result, expr);
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = scalar;
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::InsertElement: {
        ensureValueTable(value_counter);
        MSLType result_type = getTypeForInst(inst.type_id);
        if (!isUsableType(result_type) && !inst.operands.empty() &&
            inst.operands[0] < ctx.value_types.size())
            result_type = ctx.value_types[inst.operands[0]];
        if (!isUsableType(result_type) && inst.operands.size() > 1) {
            MSLTypeKind hint = inst.operands[1] < ctx.value_types.size()
                                   ? DXILIRBuilder::scalarType(
                                         ctx.value_types[inst.operands[1]])
                                         .kind
                                   : MSLTypeKind::Unknown;
            MSLType inferred = inferLongVectorType(inst.operands[0], hint);
            if (DXILIRBuilder::isLongVectorType(inferred))
                result_type = inferred;
        }
        if (inst.operands.size() >= 3) {
            auto vec = getValue(inst.operands[0]);
            auto elem = getValue(inst.operands[1]);
            auto idx = getValue(inst.operands[2]);
            if (DXILIRBuilder::isLongVectorType(result_type) &&
                (vec == "0" || vec == "undef" ||
                 exprLooksScalarLiteral(vec)))
                vec = emitTypeName(result_type) + "{}";
            emitTypedLine(result_type, result, vec);
            uint32_t idx_val = 0;
            if (parseUnsignedLiteral(idx, idx_val) &&
                idx_val < DXILIRBuilder::vectorWidth(result_type)) {
                if (DXILIRBuilder::isLongVectorType(result_type))
                    os << "  " << result << "[" << idx_val << "] = " << elem << ";\n";
                else
                    os << "  " << result << componentSuffix(idx_val) << " = " << elem << ";\n";
            } else
                os << "  " << result << "[" + idx + "] = " << elem << ";\n";
        } else {
            emitTypedLine(result_type, result, inst.operands.size() >= 1 ? getValue(inst.operands[0]) : "float4(0)");
        }
        ctx.value_table[value_counter] = result;
        ctx.value_types[value_counter] = result_type;
        value_counter++;
        break;
    }

    case LLVMInstruction::ShuffleVector: {
        ensureValueTable(value_counter);
        MSLType result_type = getTypeForInst(inst.type_id);
        auto lhs = inst.operands.size() >= 1 ? getValue(inst.operands[0]) : "float4(0)";
        auto rhs = inst.operands.size() >= 2 ? getValue(inst.operands[1]) : "float4(0)";
        auto mask = inst.operands.size() >= 3 ? getValue(inst.operands[2]) : "";
        auto mask_values = parseAggregateLiteral(mask);
        if (mask_values.empty()) {
            uint32_t si = 0;
            if (parseUnsignedLiteral(mask, si)) mask_values.push_back(mask);
        }
        if (!mask_values.empty()) {
            std::vector<std::string> components;
            for (auto &mv : mask_values) {
                uint32_t index = 0;
                if (!parseUnsignedLiteral(mv, index) || index == 0xFFFFFFFFu)
                    components.push_back("0.0f");
                else if (index < 4)
                    components.push_back("(" + lhs + ")" + componentSuffix(index));
                else
                    components.push_back("(" + rhs + ")" + componentSuffix(index - 4));
            }
            std::string type_name = emitTypeName(result_type);
            std::string expr = type_name + "(";
            for (size_t i = 0; i < components.size(); i++) {
                if (i) expr += ", ";
                expr += components[i];
            }
            expr += ")";
            emitTypedLine(result_type, result, expr);
        } else {
            emitTypedLine(result_type, result, lhs);
        }
        ctx.value_table[value_counter] = result;
        ctx.value_types[value_counter] = result_type;
        value_counter++;
        break;
    }

    case LLVMInstruction::Unreachable:
        os << "  // unreachable\n";
        break;

    case LLVMInstruction::FNeg: {
        ensureValueTable(value_counter);
        MSLType result_type = getTypeForInst(inst.type_id);
        if (inst.operands.size() >= 1) {
            MSLType op_type = inst.operands[0] < ctx.value_types.size() ? ctx.value_types[inst.operands[0]] : MSLType{};
            if (op_type.kind != MSLTypeKind::Unknown && op_type.kind != MSLTypeKind::Struct)
                result_type = op_type;
            const std::string value = getValue(inst.operands[0]);
            if (result_type.kind == MSLTypeKind::Double)
                emitTypedLine(result_type, result,
                              "(ulong(" + value + ") ^ (1ul << 63))");
            else if (DXILIRBuilder::isLongVectorType(result_type)) {
                std::string expression = emitTypeName(result_type) + "{{";
                for (uint32_t i = 0; i < result_type.vector_width; ++i) {
                    if (i)
                        expression += ", ";
                    expression += "-" + value + "[" + std::to_string(i) + "]";
                }
                expression += "}}";
                emitTypedLine(result_type, result, expression);
            } else
                emitTypedLine(result_type, result, "-(" + value + ")");
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::FAdd: case LLVMInstruction::FSub:
    case LLVMInstruction::FMul: case LLVMInstruction::FDiv: case LLVMInstruction::FRem: {
        ensureValueTable(value_counter);
        const char *fop = "+";
        switch (inst.opcode) {
        case LLVMInstruction::FAdd: fop = "+"; break;
        case LLVMInstruction::FSub: fop = "-"; break;
        case LLVMInstruction::FMul: fop = "*"; break;
        case LLVMInstruction::FDiv: fop = "/"; break;
        case LLVMInstruction::FRem: fop = "%"; break;
        default: break;
        }
        MSLType result_type = getTypeForInst(inst.type_id);
        if (inst.operands.size() >= 2) {
            MSLType tracked_lhs = valueType(inst.operands[0]);
            MSLType tracked_rhs = valueType(inst.operands[1]);
            result_type = chooseBinaryType(result_type, operandType(inst.operands[0]),
                                           operandType(inst.operands[1]), MSLTypeKind::Float);
            if (tracked_lhs.kind == MSLTypeKind::Double ||
                tracked_rhs.kind == MSLTypeKind::Double)
                result_type = {MSLTypeKind::Double, 0, {}};
            std::string lhs = coerceOperand(inst.operands[0], result_type);
            std::string rhs = coerceOperand(inst.operands[1], result_type);
            if (result_type.kind == MSLTypeKind::Double) {
                std::string expression;
                if (inst.opcode == LLVMInstruction::FAdd ||
                    inst.opcode == LLVMInstruction::FSub) {
                    if (inst.opcode == LLVMInstruction::FSub)
                        rhs = "(ulong(" + rhs + ") ^ (1ul << 63))";
                    expression = "m12_f64_add(ulong(" + lhs + "), ulong(" + rhs + "))";
                } else if (inst.opcode == LLVMInstruction::FMul) {
                    expression = "m12_f64_mul(ulong(" + lhs + "), ulong(" + rhs + "))";
                } else if (inst.opcode == LLVMInstruction::FDiv) {
                    expression = "m12_f64_div(ulong(" + lhs + "), ulong(" + rhs + "))";
                } else {
                    expression = "m12_f64_remainder(ulong(" + lhs + "), ulong(" + rhs + "))";
                }
                emitTypedLine(result_type, result, expression);
                ctx.value_table[value_counter] = result;
                ctx.value_types[value_counter] = result_type;
                value_counter++;
                break;
            }
            std::string expr;
            if (DXILIRBuilder::isLongVectorType(result_type)) {
                const MSLType lhs_type = operandType(inst.operands[0]);
                const MSLType rhs_type = operandType(inst.operands[1]);
                auto component = [&](const std::string &value, const MSLType &type,
                                     uint32_t index) {
                    if (DXILIRBuilder::isLongVectorType(type))
                        return value + "[" + std::to_string(index) + "]";
                    if (DXILIRBuilder::isVectorType(type))
                        return componentAccess(value, index, type);
                    return value;
                };
                expr = emitTypeName(result_type) + "{{";
                for (uint32_t i = 0; i < result_type.vector_width; ++i) {
                    if (i)
                        expr += ", ";
                    const std::string left = component(lhs, lhs_type, i);
                    const std::string right = component(rhs, rhs_type, i);
                    expr += inst.opcode == LLVMInstruction::FRem
                                 ? "fmod(" + left + ", " + right + ")"
                                 : left + std::string(" ") + fop + " " + right;
                }
                expr += "}}";
            } else {
                if (inst.opcode == LLVMInstruction::FRem)
                    lhs = castExpr(lhs, {MSLTypeKind::Float, 0, {}});
                if (inst.opcode == LLVMInstruction::FRem)
                    rhs = castExpr(rhs, {MSLTypeKind::Float, 0, {}});
                expr = inst.opcode == LLVMInstruction::FRem
                    ? "fmod(" + lhs + ", " + rhs + ")"
                    : lhs + std::string(" ") + fop + " " + rhs;
            }
            emitTypedLine(result_type, result, expr);
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::BitCast: {
        ensureValueTable(value_counter);
        MSLType result_type = getTypeForInst(inst.type_id);
        if (inst.operands.size() >= 1) {
            std::string val = getValue(inst.operands[0]);
            auto src_type = operandType(inst.operands[0]);
            auto dst_type = result_type;
            // LLVM type IDs are zero-based, while zero is also used as the
            // reader's unknown-type sentinel.  DXIL modules commonly place
            // i32 at type ID zero, so preserve an explicit i32 bitcast target
            // instead of falling back to the source floating-point type.
            if (!isUsableType(dst_type) && inst.type_id == 0 &&
                !ctx.mod.types.empty() &&
                ctx.mod.types[0].kind == LLVMType::Integer &&
                ctx.mod.types[0].bit_width == 32) {
                dst_type = {MSLTypeKind::Int, 0, {}};
                result_type = dst_type;
            }
            if (!isUsableType(dst_type)) {
                if (isUsableType(src_type)) {
                    dst_type = src_type;
                    result_type = src_type;
                } else {
                    dst_type = {MSLTypeKind::UInt, 0, {}};
                    result_type = dst_type;
                }
            }
            std::string src_name = emitTypeName(src_type);
            std::string dst_name = emitTypeName(dst_type);
            if ((isPointerType(src_type) || typeLooksResourceHandle(src_type)) &&
                (DXILIRBuilder::isFloatType(dst_type) || DXILIRBuilder::isIntType(dst_type))) {
                emitTypedLine(result_type, result, defaultForType(result_type));
            } else if (isPointerType(src_type) || isPointerType(dst_type)) {
                ctx.value_table[value_counter] = val;
                ctx.value_types[value_counter] = dst_type;
                value_counter++;
                break;
            } else if (DXILIRBuilder::isVectorType(src_type) &&
                       (DXILIRBuilder::isFloatType(dst_type) || DXILIRBuilder::isIntType(dst_type)) &&
                       !DXILIRBuilder::isVectorType(dst_type)) {
                emitTypedLine(result_type, result, castExpr(componentAccess(val, 0, src_type), dst_type));
            } else if (src_name != dst_name && !src_name.empty() && !dst_name.empty() &&
                src_type.kind != MSLTypeKind::Unknown && dst_type.kind != MSLTypeKind::Unknown &&
                DXILIRBuilder::typeBitWidth(src_type) == DXILIRBuilder::typeBitWidth(dst_type)) {
                emitTypedLine(result_type, result, "as_type<" + dst_name + ">(" + val + ")");
            } else if (src_name != dst_name && !src_name.empty() && !dst_name.empty() &&
                       src_type.kind != MSLTypeKind::Unknown && dst_type.kind != MSLTypeKind::Unknown) {
                emitTypedLine(result_type, result, castExpr(val, dst_type));
            } else {
                emitTypedLine(result_type, result, val);
            }
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::ZExt: case LLVMInstruction::SExt: case LLVMInstruction::Trunc:
    case LLVMInstruction::FPToUI: case LLVMInstruction::FPToSI:
    case LLVMInstruction::UIToFP: case LLVMInstruction::SIToFP:
    case LLVMInstruction::FPTrunc: case LLVMInstruction::FPExt: {
        ensureValueTable(value_counter);
        MSLType result_type = getTypeForInst(inst.type_id);
        if (inst.operands.size() >= 1) {
            std::string val = getValue(inst.operands[0]);
            MSLType source_type = operandType(inst.operands[0]);
            if (!isUsableType(result_type))
                result_type = {MSLTypeKind::Int, 0, {}};
            std::string dst_name = emitTypeName(result_type);
            if (source_type.kind == MSLTypeKind::Double) {
                std::string converted;
                if (result_type.kind == MSLTypeKind::Float)
                    converted = "m12_f64_to_float(ulong(" + val + "))";
                else if (result_type.kind == MSLTypeKind::Long)
                    converted = "m12_f64_to_slong(ulong(" + val + "))";
                else if (result_type.kind == MSLTypeKind::UInt)
                    converted = "m12_f64_to_uint(ulong(" + val + "))";
                else
                    converted = "m12_f64_to_sint(ulong(" + val + "))";
                emitTypedLine(result_type, result, converted);
            } else if (result_type.kind == MSLTypeKind::Double) {
                std::string converted;
                if (source_type.kind == MSLTypeKind::Float ||
                    source_type.kind == MSLTypeKind::Half)
                    converted = "m12_f64_from_float(static_cast<float>(" + val + "))";
                else if (inst.opcode == LLVMInstruction::UIToFP)
                    converted = "m12_f64_from_uint(ulong(" + val + "))";
                else
                    converted = "m12_f64_from_sint(long(" + val + "))";
                emitTypedLine(result_type, result, converted);
            } else if (isPointerType(source_type) || typeLooksResourceHandle(source_type) ||
                exprLooksResourceHandle(val) || exprContainsRawResourceHandle(val))
                emitTypedLine(result_type, result, defaultForType(result_type));
            else if (DXILIRBuilder::isVectorType(source_type) &&
                     (DXILIRBuilder::isFloatType(result_type) || DXILIRBuilder::isIntType(result_type)) &&
                     !DXILIRBuilder::isVectorType(result_type))
                emitTypedLine(result_type, result, castExpr(componentAccess(val, 0, source_type), result_type));
            else
                emitTypedLine(result_type, result, "static_cast<" + dst_name + ">(" + val + ")");
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::PtrToInt: {
        ensureValueTable(value_counter);
        MSLType result_type = getTypeForInst(inst.type_id);
        if (inst.operands.size() >= 1) {
            std::string val = getValue(inst.operands[0]);
            MSLType source_type = operandType(inst.operands[0]);
            if (!isUsableType(result_type))
                result_type = {MSLTypeKind::UInt, 0, {}};
            if (isPointerType(source_type) || typeLooksResourceHandle(source_type) ||
                exprLooksResourceHandle(val) || exprContainsRawResourceHandle(val))
                emitTypedLine(result_type, result, defaultForType(result_type));
            else
                emitTypedLine(result_type, result, val);
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::IntToPtr: {
        ensureValueTable(value_counter);
        if (inst.operands.size() >= 1) {
            std::string val = getValue(inst.operands[0]);
            MSLType source_type = operandType(inst.operands[0]);
            MSLType name_type = typeForResolvedValueName(ctx, val);
            if (DXILIRBuilder::isVectorType(source_type) ||
                DXILIRBuilder::isVectorType(name_type) ||
                exprLooksVectorValue(val) ||
                exprLooksScalarMathCall(val) ||
                exprLooksScalarCast(val) ||
                exprLooksScalarLiteral(val))
                val = "0";
            ctx.value_table[value_counter] = val;
            ctx.value_types[value_counter] = {MSLTypeKind::DeviceCharPtr, 0, {}};
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::FCmp: case LLVMInstruction::ICmp: {
        ensureValueTable(value_counter);
        if (inst.operands.size() >= 3) {
            uint32_t pred = inst.operands[0];
            MSLType lhs_type = operandType(inst.operands[1]);
            MSLType rhs_type = operandType(inst.operands[2]);
            MSLType cmp_type = chooseBinaryType(
                inst.opcode == LLVMInstruction::FCmp ? MSLType{MSLTypeKind::Float, 0, {}}
                                                      : MSLType{MSLTypeKind::Int, 0, {}},
                lhs_type, rhs_type,
                inst.opcode == LLVMInstruction::FCmp ? MSLTypeKind::Float : MSLTypeKind::Int);
            auto lhs = coerceOperand(inst.operands[1], cmp_type);
            auto rhs = coerceOperand(inst.operands[2], cmp_type);
            if (inst.opcode == LLVMInstruction::FCmp &&
                cmp_type.kind == MSLTypeKind::Double) {
                MSLType bool_type = {MSLTypeKind::Bool, 0, {}};
                emitTypedLine(bool_type, result,
                              "m12_f64_cmp(ulong(" + lhs + "), ulong(" + rhs + "), " +
                                  std::to_string(pred) + "u)");
                ctx.value_table[value_counter] = result;
                ctx.value_types[value_counter] = bool_type;
                value_counter++;
                break;
            }
            const char *cmp = "==";
            MSLType result_type = {MSLTypeKind::Bool, 0, {}};
            std::string cmp_result;
            const bool long_vector_cmp =
                DXILIRBuilder::isLongVectorType(cmp_type);
            if (long_vector_cmp) {
                switch (pred) {
                case 0: cmp_result = "false"; break;
                case 15: cmp_result = "true"; break;
                case 7:
                case 8:
                case 14:
                    break;
                default:
                    if (pred == 1 || pred == 8 || pred == 32) cmp = "==";
                    else if (pred == 2 || pred == 9 || pred == 34 || pred == 38) cmp = ">";
                    else if (pred == 3 || pred == 10 || pred == 35 || pred == 39) cmp = ">=";
                    else if (pred == 4 || pred == 11 || pred == 36 || pred == 40) cmp = "<";
                    else if (pred == 5 || pred == 12 || pred == 37 || pred == 41) cmp = "<=";
                    else if (pred == 6 || pred == 13 || pred == 33) cmp = "!=";
                    break;
                }
                if (pred != 0 && pred != 15) {
                    const MSLType lhs_vector_type = operandType(inst.operands[1]);
                    const MSLType rhs_vector_type = operandType(inst.operands[2]);
                    const std::string lhs_value = getValue(inst.operands[1]);
                    const std::string rhs_value = getValue(inst.operands[2]);
                    auto component = [&](const std::string &value,
                                         const MSLType &type, uint32_t index) {
                        if (DXILIRBuilder::isLongVectorType(type))
                            return value + "[" + std::to_string(index) + "]";
                        if (DXILIRBuilder::isVectorType(type))
                            return componentAccess(value, index, type);
                        return value;
                    };
                    result_type = cmp_type;
                    result_type.vector_element_kind = MSLTypeKind::Int;
                    const std::string type_name = emitTypeName(result_type);
                    cmp_result = type_name + "{{";
                    for (uint32_t i = 0; i < result_type.vector_width; ++i) {
                        if (i)
                            cmp_result += ", ";
                        const std::string lhs = component(lhs_value,
                                                          lhs_vector_type, i);
                        const std::string rhs = component(rhs_value,
                                                          rhs_vector_type, i);
                        if (pred == 7)
                            cmp_result += "(!isnan(" + lhs + ") && !isnan(" + rhs + ")) ? 1 : 0";
                        else if (pred == 14)
                            cmp_result += "(isnan(" + lhs + ") || isnan(" + rhs + ")) ? 1 : 0";
                        else
                            cmp_result += "(" + lhs + " " + cmp + " " + rhs + ") ? 1 : 0";
                    }
                    cmp_result += "}}";
                }
            }
            if (!long_vector_cmp && inst.opcode == LLVMInstruction::ICmp) {
                switch (pred) {
                case 32: cmp = "=="; break; // ICMP_EQ
                case 33: cmp = "!="; break; // ICMP_NE
                case 34: cmp = ">"; break;  // ICMP_UGT
                case 35: cmp = ">="; break; // ICMP_UGE
                case 36: cmp = "<"; break;  // ICMP_ULT
                case 37: cmp = "<="; break; // ICMP_ULE
                case 38: cmp = ">"; break;  // ICMP_SGT
                case 39: cmp = ">="; break; // ICMP_SGE
                case 40: cmp = "<"; break;  // ICMP_SLT
                case 41: cmp = "<="; break; // ICMP_SLE
                default:
                    ctx.unsupported_opcodes++;
                    cmp_result = "false";
                    break;
                }
                if (cmp_result.empty()) {
                    std::string cmp_expr =
                        "(" + lhs + " " + cmp + " " + rhs + ")";
                    bool vector_cmp = DXILIRBuilder::isVectorType(cmp_type) ||
                                      exprLooksVectorValue(lhs) ||
                                      exprLooksVectorValue(rhs) ||
                                      exprContainsBareVectorTypedValue(ctx, lhs) ||
                                      exprContainsBareVectorTypedValue(ctx, rhs);
                    cmp_result = vector_cmp
                                     ? std::string("any((") + cmp_expr + "))"
                                     : cmp_expr;
                }
            } else if (!long_vector_cmp && pred == 0) {
                cmp_result = "false";
            } else if (!long_vector_cmp && pred == 15) {
                cmp_result = "true";
            } else if (!long_vector_cmp && pred == 7) {
                std::string ilhs = coerceIsNanOperand(lhs, cmp_type);
                std::string irhs = coerceIsNanOperand(rhs, cmp_type);
                if (DXILIRBuilder::isVectorType(cmp_type))
                    cmp_result = "all((!isnan(" + ilhs + ")) & (!isnan(" + irhs + ")))";
                else
                    cmp_result = "(!isnan(" + ilhs + ") && !isnan(" + irhs + "))";
            } else if (!long_vector_cmp && pred == 14) {
                std::string ilhs = coerceIsNanOperand(lhs, cmp_type);
                std::string irhs = coerceIsNanOperand(rhs, cmp_type);
                if (DXILIRBuilder::isVectorType(cmp_type))
                    cmp_result = "any((isnan(" + ilhs + ")) | (isnan(" + irhs + ")))";
                else
                    cmp_result = "(isnan(" + ilhs + ") || isnan(" + irhs + "))";
            } else if (!long_vector_cmp) {
                if (pred == 1 || pred == 8) cmp = "==";
                else if (pred == 2 || pred == 9) cmp = ">";
                else if (pred == 3 || pred == 10) cmp = ">=";
                else if (pred == 4 || pred == 11) cmp = "<";
                else if (pred == 5 || pred == 12) cmp = "<=";
                else if (pred == 6 || pred == 13) cmp = "!=";
                std::string cmp_expr = "(" + lhs + " " + cmp + " " + rhs + ")";
                bool vector_cmp = DXILIRBuilder::isVectorType(cmp_type) ||
                                  exprLooksVectorValue(lhs) ||
                                  exprLooksVectorValue(rhs) ||
                                  exprContainsBareVectorTypedValue(ctx, lhs) ||
                                  exprContainsBareVectorTypedValue(ctx, rhs);
                cmp_result = vector_cmp ? std::string("any((") + cmp_expr + "))" : cmp_expr;
            }
            emitTypedLine(result_type, result, cmp_result);
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::Select: {
        ensureValueTable(value_counter);
        MSLType result_type = getTypeForInst(inst.type_id);
        if (inst.operands.size() >= 3) {
            const MSLType condition_type = operandType(inst.operands[0]);
            MSLType tv_type = operandType(inst.operands[1]);
            MSLType fv_type = operandType(inst.operands[2]);
            result_type = chooseBinaryType(result_type, tv_type, fv_type, MSLTypeKind::Int);
            if (DXILIRBuilder::isVectorType(tv_type) ||
                DXILIRBuilder::isLongVectorType(tv_type))
                result_type = tv_type;
            else if (DXILIRBuilder::isVectorType(fv_type) ||
                     DXILIRBuilder::isLongVectorType(fv_type))
                result_type = fv_type;
            auto pre_it = ctx.predeclared_types.find(result);
            if (pre_it != ctx.predeclared_types.end() &&
                (DXILIRBuilder::isFloatType(pre_it->second) || DXILIRBuilder::isIntType(pre_it->second)) &&
                !DXILIRBuilder::isVectorType(pre_it->second) &&
                !DXILIRBuilder::isLongVectorType(pre_it->second))
                result_type = pre_it->second;
            if (DXILIRBuilder::isLongVectorType(condition_type) &&
                DXILIRBuilder::isLongVectorType(result_type)) {
                const std::string cond = getValue(inst.operands[0]);
                const std::string true_value = getValue(inst.operands[1]);
                const std::string false_value = getValue(inst.operands[2]);
                const bool true_vector = DXILIRBuilder::isLongVectorType(tv_type) ||
                                         startsWith(true_value, "array<");
                const bool false_vector = DXILIRBuilder::isLongVectorType(fv_type) ||
                                          startsWith(false_value, "array<");
                std::string expression = emitTypeName(result_type) + "{{";
                for (uint32_t i = 0; i < result_type.vector_width; ++i) {
                    if (i)
                        expression += ", ";
                    const std::string tv = true_vector
                                               ? true_value + "[" + std::to_string(i) + "]"
                                               : true_value;
                    const std::string fv = false_vector
                                               ? false_value + "[" + std::to_string(i) + "]"
                                               : false_value;
                    expression += "(" + cond + "[" + std::to_string(i) + "] != 0 ? " +
                                  tv + " : " + fv + ")";
                }
                expression += "}}";
                emitTypedLine(result_type, result, expression);
            } else {
                auto cond = coerceOperand(inst.operands[0], {MSLTypeKind::Bool, 0, {}});
                auto tv = coerceOperand(inst.operands[1], result_type);
                auto fv = coerceOperand(inst.operands[2], result_type);
                emitTypedLine(result_type, result, "(" + cond + " ? " + tv + " : " + fv + ")");
            }
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
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
        if (ctx.predeclared_allocas.find(value_counter) == ctx.predeclared_allocas.end())
            os << "  " << storage_class << " char " << alloca_name << "[256];\n";
        ensureValueTable(value_counter);
        ctx.value_table[value_counter] = "(" + storage_class + " char*)&" + alloca_name;
        ctx.value_types[value_counter] = {MSLTypeKind::DeviceCharPtr, 0, {}};
        if (inst.type_id < ctx.mod.types.size()) {
            const auto &allocated_type = ctx.mod.types[inst.type_id];
            MSLType pointee;
            if (allocated_type.kind == LLVMType::Pointer &&
                !allocated_type.type_refs.empty())
                pointee = DXILIRBuilder::resolveType(allocated_type.type_refs[0], ctx.mod);
            else
                pointee = DXILIRBuilder::resolveType(inst.type_id, ctx.mod);
            if (isUsableType(pointee))
                ctx.pointer_pointee_types[value_counter] = pointee;
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::AtomicRMW: {
        ensureValueTable(value_counter);
        MSLType result_type = getTypeForInst(inst.type_id);
        if (!isUsableType(result_type))
            result_type = {MSLTypeKind::Long, 0, {}};
        const bool is_i64 =
            inst.type_id < ctx.mod.types.size() &&
            ctx.mod.types[inst.type_id].kind == LLVMType::Integer &&
            ctx.mod.types[inst.type_id].bit_width == 64;
        if (inst.operands.size() >= 3 && is_i64) {
            auto ptr = getValue(inst.operands[0]);
            auto val = getValue(inst.operands[1]);
            uint32_t llvm_op = inst.operands[2];
            uint32_t m12_op = 0;
            switch (llvm_op) {
            case 0: m12_op = 8; break;
            case 1: m12_op = 0; break;
            case 2: m12_op = 9; break;
            case 3: m12_op = 1; break;
            case 5: m12_op = 2; break;
            case 6: m12_op = 3; break;
            case 7: m12_op = 5; break;
            case 8: m12_op = 4; break;
            case 9: m12_op = 7; break;
            case 10: m12_op = 6; break;
            default: m12_op = 0; break;
            }
            std::string translated =
                "(long)m12_atomic64_binop_group("
                "reinterpret_cast<volatile threadgroup ulong*>(" +
                ptr + "), (ulong)(" + val + "), " +
                std::to_string(m12_op) +
                "u, &m12_atomic64_group_lock)";
            if (ctx.predeclared_names.find(result) !=
                ctx.predeclared_names.end())
                os << "  " << result << " = " << translated << ";\n";
            else
                os << "  " << emitTypeName(result_type) << " " << result
                   << " = " << translated << ";\n";
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
        } else if (inst.operands.size() >= 3) {
            result_type = {MSLTypeKind::UInt, 0, {}};
            auto ptr = getValue(inst.operands[0]);
            auto val = getValue(inst.operands[1]);
            uint32_t llvm_op = inst.operands[2];
            std::string translated;
            if (llvm_op == 1)
                translated =
                    "atomic_fetch_add_explicit(reinterpret_cast<threadgroup atomic_uint*>(" +
                    ptr + "), (uint)(" + val + "), memory_order_relaxed)";
            else
                translated = "0u";
            if (ctx.predeclared_names.find(result) !=
                ctx.predeclared_names.end())
                os << "  " << result << " = " << translated << ";\n";
            else
                os << "  uint " << result << " = " << translated << ";\n";
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
        } else {
            ctx.unsupported_opcodes++;
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::CmpXchg: {
        ensureValueTable(value_counter);
        MSLType result_type = {MSLTypeKind::Long, 0, {}};
        const bool is_i64 =
            inst.type_id < ctx.mod.types.size() &&
            ctx.mod.types[inst.type_id].kind == LLVMType::Integer &&
            ctx.mod.types[inst.type_id].bit_width == 64;
        if (inst.operands.size() >= 3 && is_i64) {
            auto ptr = getValue(inst.operands[0]);
            auto compare_value = getValue(inst.operands[1]);
            auto new_value = getValue(inst.operands[2]);
            std::string translated =
                "(long)m12_atomic64_compare_exchange_group("
                "reinterpret_cast<volatile threadgroup ulong*>(" +
                ptr + "), (ulong)(" + compare_value + "), (ulong)(" +
                new_value + "), &m12_atomic64_group_lock)";
            if (ctx.predeclared_names.find(result) !=
                ctx.predeclared_names.end())
                os << "  " << result << " = " << translated << ";\n";
            else
                os << "  long " << result << " = " << translated << ";\n";
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
        } else {
            ctx.unsupported_opcodes++;
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::Load: {
        ensureValueTable(value_counter);
        MSLType result_type = getTypeForInst(inst.type_id);
        if (inst.operands.size() >= 1) {
            auto ptr = getValue(inst.operands[0]);
            auto ptr_type = valueType(inst.operands[0]);
            auto ptr_name_type = typeForResolvedValueName(ctx, ptr);
            const bool group_i64 =
                ctx.group_i64_globals.find(inst.operands[0]) !=
                ctx.group_i64_globals.end();
            if (group_i64) {
                result_type = {MSLTypeKind::Long, 0, {}};
                std::string expr =
                    "(long)(*reinterpret_cast<threadgroup ulong*>(" + ptr + "))";
                if (ctx.predeclared_names.find(result) !=
                    ctx.predeclared_names.end())
                    os << "  " << result << " = " << expr << ";\n";
                else
                    os << "  long " << result << " = " << expr << ";\n";
                ctx.value_table[value_counter] = result;
                ctx.value_types[value_counter] = result_type;
                value_counter++;
                break;
            }
            if (!isUsableType(result_type))
                result_type = {MSLTypeKind::UInt, 0, {}};
            auto pointee_it = ctx.pointer_pointee_types.find(inst.operands[0]);
            if (pointee_it != ctx.pointer_pointee_types.end() &&
                isUsableType(pointee_it->second))
                result_type = pointee_it->second;
            std::string type_name = emitTypeName(result_type);
            std::string expr = defaultForType(result_type);
            const bool pointer_expression =
                isPointerType(ptr_type) || exprContainsPointerSyntax(ptr) ||
                ptr.find("threadgroup") != std::string::npos ||
                ptr.find("device") != std::string::npos ||
                ptr.find("gvar_") != std::string::npos;
            if (pointer_expression) {
                expr = "*reinterpret_cast<" + std::string(pointerAddressSpace(inst.operands[0])) + " " +
                       type_name + "*>(" + ptr + ")";
            } else if (startsWith(ptr, "tex") || startsWith(ptr, "samp") ||
                       exprContainsRawResourceHandle(ptr) || exprLooksScalarLiteral(ptr) ||
                       exprLooksVectorValue(ptr) || DXILIRBuilder::isVectorType(ptr_name_type)) {
                expr = defaultForType(result_type);
            }
            if (ptr.find("gvar_") != std::string::npos) {
                std::string group_expr = "static_cast<" + type_name + ">(*reinterpret_cast<threadgroup uint*>(" + ptr + "))";
                if (ctx.predeclared_names.find(result) !=
                    ctx.predeclared_names.end())
                    os << "  " << result << " = " << group_expr << ";\n";
                else
                    os << "  " << type_name << " " << result << " = "
                       << group_expr << ";\n";
            } else {
                emitTypedLine(result_type, result, expr);
            }
            ctx.value_table[value_counter] = result;
            ctx.value_types[value_counter] = result_type;
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::Store: {
        if (inst.operands.size() >= 2) {
            auto ptr = getValue(inst.operands[0]);
            auto val = getValue(inst.operands[1]);
            auto ptr_type = valueType(inst.operands[0]);
            auto val_type = operandType(inst.operands[1]);
            if (isUsableType(val_type) &&
                (DXILIRBuilder::isVectorType(val_type) ||
                 DXILIRBuilder::isLongVectorType(val_type)))
                ctx.pointer_pointee_types[inst.operands[0]] = val_type;
            MSLType ptr_name_type = typeForResolvedValueName(ctx, ptr);
            const bool group_i64 =
                ctx.group_i64_globals.find(inst.operands[0]) !=
                ctx.group_i64_globals.end();
            if (startsWith(ptr, "tex") || startsWith(ptr, "samp") ||
                exprContainsRawResourceHandle(ptr) || exprLooksScalarLiteral(ptr)) {
                os << "  // skipped store through resource handle " << ptr << "\n";
            } else if (isPointerType(val_type) || typeLooksResourceHandle(val_type) ||
                       exprLooksResourceHandle(val) || exprContainsRawResourceHandle(val)) {
                os << "  // skipped store of pointer/resource value " << val << "\n";
            } else if (exprLooksVectorValue(ptr) || DXILIRBuilder::isVectorType(ptr_name_type)) {
                os << "  // skipped store through vector-valued pointer " << ptr << "\n";
            } else if (isPointerType(ptr_type)) {
                std::string type_name = group_i64 ? "ulong" : emitTypeName(val_type);
                if (type_name.empty() || type_name == "auto" || type_name == "void") type_name = "uint";
                os << "  *((" << pointerAddressSpace(inst.operands[0]) << " " << type_name
                   << "*)(" << ptr << ")) = ";
                if (group_i64)
                    os << "(ulong)(" << val << ")";
                else
                    os << val;
                os << ";\n";
            } else {
                os << "  // skipped store through non-pointer " << ptr << "\n";
            }
        }
        break;
    }

    case LLVMInstruction::GetElementPtr: {
        ensureValueTable(value_counter);
        if (inst.operands.empty()) {
            ctx.value_table[value_counter] = "0";
            ctx.value_types[value_counter] = {MSLTypeKind::DeviceCharPtr, 0, {}};
            value_counter++;
            break;
        }
        auto base = getValue(inst.operands[0]);
        auto base_type = operandType(inst.operands[0]);
        std::string gep = base;
        if (startsWith(base, "tex") || startsWith(base, "samp") ||
            exprContainsRawResourceHandle(base)) {
            ctx.value_table[value_counter] = "0";
            ctx.value_types[value_counter] = {MSLTypeKind::DeviceCharPtr, 0, {}};
            value_counter++;
            break;
        }
        if (!isPointerType(base_type) && !typeLooksResourceHandle(base_type) &&
            !exprContainsPointerSyntax(base)) {
            ctx.value_table[value_counter] = "0";
            ctx.value_types[value_counter] = {MSLTypeKind::DeviceCharPtr, 0, {}};
            value_counter++;
            break;
        }
        for (size_t i = 1; i < inst.operands.size(); i++) {
            auto idx = coerceOperand(inst.operands[i], {MSLTypeKind::Int, 0, {}});
            idx = ensureScalarIndex(idx);
            if (idx != "0" && idx != "0.0" && idx != "0.0f") {
                // The translated alloca storage is byte-addressed. LLVM GEP
                // indices are element-addressed, so scale the selected
                // element by the scalar slot size before casting the pointer.
                // DXIL's private aggregate/vector scratch values are built
                // from 32-bit lanes; zero outer indices remain free.
                gep += " + (" + idx + ") * 4";
            }
        }
        ctx.value_table[value_counter] = gep;
        ctx.value_types[value_counter] = {MSLTypeKind::DeviceCharPtr, 0, {}};
        auto base_pointee_it = ctx.pointer_pointee_types.find(inst.operands[0]);
        if (base_pointee_it != ctx.pointer_pointee_types.end()) {
            MSLType pointee = base_pointee_it->second;
            if ((DXILIRBuilder::isVectorType(pointee) ||
                 DXILIRBuilder::isLongVectorType(pointee)) &&
                inst.operands.size() >= 2)
                pointee = DXILIRBuilder::scalarType(pointee);
            if (isUsableType(pointee))
                ctx.pointer_pointee_types[value_counter] = pointee;
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::PHI: {
        ensureValueTable(value_counter);
        ctx.value_table[value_counter] = result;
        ctx.value_types[value_counter] = getTypeForInst(inst.type_id);
        value_counter++;
        break;
    }

    case LLVMInstruction::Invoke:
        os << "  // invoke\n";
        break;

    default:
        ctx.unsupported_opcodes++;
        os << "  // unhandled opcode " << (int)inst.opcode << "\n";
        ensureValueTable(value_counter);
        ctx.value_table[value_counter] = result;
        value_counter++;
        break;
    }
}

std::optional<TypedMSLShader> MSLLowering::lower(
    const LLVMModule &module, const DxilParsedShader &shader,
    const MSLLoweringOptions &options) {
    if (module.functions.empty()) return std::nullopt;

    std::ostringstream os;
    LowerContext ctx{os, module, shader, options};
    ctx.compute_wave_shader = shader.kind == DxilShaderKind::Compute;
    if (ctx.compute_wave_shader) {
        ctx.compute_wave_shader = false;
        for (const auto &decl : module.functions) {
            if (startsWith(decl.name, "dx.op.wave")) {
                ctx.compute_wave_shader = true;
                break;
            }
        }
    }
    ctx.compute_raw_gather_shader = shader.kind == DxilShaderKind::Compute;
    if (ctx.compute_raw_gather_shader) {
        ctx.compute_raw_gather_shader = false;
        for (const auto &decl : module.functions) {
            if (startsWith(decl.name, "dx.op.textureGatherRaw")) {
                ctx.compute_raw_gather_shader = true;
                break;
            }
        }
    }
    ctx.compute_texture_store_shader = shader.kind == DxilShaderKind::Compute;
    if (ctx.compute_texture_store_shader) {
        ctx.compute_texture_store_shader = false;
        for (const auto &decl : module.functions) {
            if (startsWith(decl.name, "dx.op.textureStore")) {
                ctx.compute_texture_store_shader = true;
                break;
            }
        }
    }
    for (const auto &decl : module.functions) {
        if (startsWith(decl.name, "dx.op.textureStoreSample")) {
            ctx.texture_store_sample_shader = true;
            break;
        }
    }
    ctx.sample_cmp_shader = false;
    for (const auto &decl : module.functions) {
        if (startsWith(decl.name, "dx.op.sampleCmp") ||
            startsWith(decl.name, "dx.op.textureGatherCmp")) {
            ctx.sample_cmp_shader = true;
            break;
        }
    }
    ctx.compute_sample_cmp_shader = shader.kind == DxilShaderKind::Compute &&
                                    ctx.sample_cmp_shader;
    ctx.compute_texture_sample_shader = shader.kind == DxilShaderKind::Compute;
    if (ctx.compute_texture_sample_shader) {
        ctx.compute_texture_sample_shader = false;
        for (const auto &decl : module.functions) {
            if (startsWith(decl.name, "dx.op.sample") ||
                startsWith(decl.name, "dx.op.textureGather") ||
                startsWith(decl.name, "dx.op.calculateLOD")) {
                ctx.compute_texture_sample_shader = true;
                break;
            }
        }
    }
    ctx.uses_atomic64_emulation = shader.kind == DxilShaderKind::Compute;
    if (ctx.uses_atomic64_emulation) {
        ctx.uses_atomic64_emulation = false;
        for (const auto &decl : module.functions) {
            if (decl.name.find("dx.op.atomicBinOp.i64") !=
                    std::string::npos ||
                decl.name.find("dx.op.atomicCompareExchange.i64") !=
                    std::string::npos) {
                ctx.uses_atomic64_emulation = true;
                break;
            }
        }
    }
    ctx.uses_atomic32_emulation = shader.kind == DxilShaderKind::Compute;
    if (ctx.uses_atomic32_emulation) {
        ctx.uses_atomic32_emulation = false;
        for (const auto &decl : module.functions) {
            if (decl.name.find("dx.op.atomicBinOp.i32") !=
                    std::string::npos ||
                decl.name.find("dx.op.atomicCompareExchange.i32") !=
                    std::string::npos) {
                ctx.uses_atomic32_emulation = true;
                break;
            }
        }
    }
    ctx.uses_sampler_feedback = options.sampler_feedback;
    for (const auto &decl : module.functions) {
        if (decl.name.find("dx.op.writeSamplerFeedback") !=
            std::string::npos) {
            ctx.uses_sampler_feedback = true;
            break;
        }
    }
    if (shader.kind == DxilShaderKind::Compute) {
        for (const auto &candidate : module.functions) {
            for (const auto &block : candidate.blocks) {
                for (const auto &inst : block.instructions) {
                    if (inst.opcode != LLVMInstruction::AtomicRMW &&
                        inst.opcode != LLVMInstruction::CmpXchg)
                        continue;
                    const bool is_i64 =
                        inst.type_id < module.types.size() &&
                        module.types[inst.type_id].kind == LLVMType::Integer &&
                        module.types[inst.type_id].bit_width == 64;
                    if (is_i64) {
                        ctx.uses_atomic64_emulation = true;
                        ctx.uses_group_atomic64_emulation = true;
                    }
                }
            }
        }
    }

    const LLVMFunction *entry_fn = nullptr;
    for (auto &fn : module.functions) {
        if (!fn.blocks.empty() && !shader.entry_point.empty() && fn.name == shader.entry_point) {
            entry_fn = &fn; break;
        }
    }
    if (!entry_fn) {
        for (auto it = module.functions.rbegin(); it != module.functions.rend(); ++it) {
            if (!it->blocks.empty()) { entry_fn = &*it; break; }
        }
    }
    if (!entry_fn) return std::nullopt;

    auto &fn = *entry_fn;
    ctx.current_fn = entry_fn;

    ctx.value_table.resize(256);
    ctx.value_types.resize(256);

    auto loadConstants = [&](const std::vector<LLVMValue> &consts) {
        for (auto &c : consts) {
            uint32_t val_idx = c.id;
            if (ctx.value_table.size() <= val_idx) ctx.value_table.resize(val_idx + 1);
            if (ctx.value_types.size() <= val_idx) ctx.value_types.resize(val_idx + 1);
            std::string cdata = c.constant_data;
            if (startsWith(cdata, "agg(")) {
                MSLType type = DXILIRBuilder::resolveType(c.type_id, module);
                ctx.value_table[val_idx] = aggregateConstructor(cdata, type);
                ctx.value_types[val_idx] = type;
                continue;
            }
            ctx.value_table[val_idx] = cdata.empty() ? "const_" + std::to_string(val_idx)
                                                     : normalizeAggregateExpressions(cdata);
            ctx.value_types[val_idx] = DXILIRBuilder::resolveType(c.type_id, module);
        }
    };

    loadConstants(module.constants);
    loadConstants(fn.constants);

    for (auto &dfn : module.functions) {
        if (!dfn.is_declaration || dfn.name.empty()) continue;
        if (ctx.value_table.size() <= dfn.value_id) ctx.value_table.resize(dfn.value_id + 1);
        if (ctx.value_types.size() <= dfn.value_id) ctx.value_types.resize(dfn.value_id + 1);
        ctx.value_table[dfn.value_id] = dfn.name;
        ctx.value_types[dfn.value_id] = {MSLTypeKind::Unknown, 0, {}};
        ctx.function_decls[dfn.value_id] = dfn.name;
    }
    for (const auto &decl : ctx.function_decls) {
        if (decl.second.find("dx.op.writeSamplerFeedback") !=
            std::string::npos) {
            ctx.uses_sampler_feedback = true;
            break;
        }
    }
    for (const auto &decl : ctx.function_decls) {
        if (decl.second.find("dx.op.tempRegLoad") != std::string::npos ||
            decl.second.find("dx.op.tempRegStore") != std::string::npos ||
            decl.second.find("dx.op.minPrecXRegLoad") != std::string::npos ||
            decl.second.find("dx.op.minPrecXRegStore") != std::string::npos) {
            ctx.uses_temp_registers = true;
            break;
        }
    }

    bool module_has_double = false;
    for (const auto &decl : ctx.function_decls) {
        if (decl.second.find("makeDouble") != std::string::npos ||
            decl.second.find("splitDouble") != std::string::npos ||
            decl.second.find("bitcastI64toF64") != std::string::npos ||
            decl.second.find("bitcastF64toI64") != std::string::npos) {
            module_has_double = true;
            break;
        }
    }
    for (uint32_t type_id = 0;
         !module_has_double && type_id < module.types.size(); ++type_id) {
        if (DXILIRBuilder::resolveType(type_id, module).kind ==
            MSLTypeKind::Double) {
            module_has_double = true;
            break;
        }
    }
    if (module_has_double)
        ctx.uses_double_emulation = true;

    if (shader.kind == DxilShaderKind::Pixel) {
        for (const auto &block : fn.blocks) {
            for (const auto &inst : block.instructions) {
                if (inst.opcode != LLVMInstruction::Call || inst.operands.empty())
                    continue;
                auto decl = ctx.function_decls.find(inst.operands[0]);
                if (decl == ctx.function_decls.end())
                    continue;
                const uint32_t intrinsic = intrinsicIdFromCalleeName(decl->second);
                if (intrinsic == DXOP_SampleIndex)
                    ctx.uses_sample_index = true;
                else if (intrinsic == DXOP_Coverage)
                    ctx.uses_coverage = true;
                else if (intrinsic == DXOP_EvalSnapped ||
                         intrinsic == DXOP_EvalSampleIndex ||
                         intrinsic == DXOP_EvalCentroid)
                    ctx.uses_interpolation = true;
            }
        }
    }

    analyzeBindingPlan(ctx, fn);
    analyzeVertexInputs(ctx, fn);
    bool has_argumentless_load_input_f32_decl =
        hasArgumentlessLoadInputF32Declaration(module);
    ctx.vertex_procedural_fullscreen_fallback =
        shader.kind == DxilShaderKind::Vertex &&
        ctx.options.vertex_inputs.empty() &&
        (has_argumentless_load_input_f32_decl ||
         hasArgumentlessLoadInputF32(ctx, fn));
    if (ctx.vertex_procedural_fullscreen_fallback) {
        recordDiagnostic(ctx,
                         "vertex procedural fullscreen fallback active load_input_decl=%u",
                         has_argumentless_load_input_f32_decl ? 1u : 0u);
    }
    emitFunctionPrologue(ctx);

    for (auto &gv : module.globals) {
        if (gv.address_space == 3) {
            std::string gv_name = gv.name.empty() ? "gvar_" + std::to_string(gv.value_id) : escapeName(gv.name);
            const bool is_i64 =
                gv.type_id < module.types.size() &&
                module.types[gv.type_id].kind == LLVMType::Integer &&
                module.types[gv.type_id].bit_width == 64;
            if (is_i64) {
                os << "  threadgroup ulong " << gv_name << ";\n";
                ctx.group_i64_globals.insert(gv.value_id);
            } else {
                os << "  threadgroup char " << gv_name << "[256];\n";
            }
            if (ctx.value_table.size() <= gv.value_id) ctx.value_table.resize(gv.value_id + 1);
            ctx.value_table[gv.value_id] = "(threadgroup char*)&" + gv_name;
            if (ctx.value_types.size() <= gv.value_id) ctx.value_types.resize(gv.value_id + 1);
            ctx.value_types[gv.value_id] = {MSLTypeKind::ThreadgroupCharPtr, 0, {}};
        }
    }

    auto seedValue = [&](uint32_t value_id, const std::string &expr, MSLType type,
                         ValueRole role = ValueRole::Generic) {
        if (ctx.value_table.size() <= value_id) ctx.value_table.resize(value_id + 1);
        if (ctx.value_types.size() <= value_id) ctx.value_types.resize(value_id + 1);
        if (ctx.value_roles.size() <= value_id) ctx.value_roles.resize(value_id + 1);
        ctx.value_table[value_id] = expr;
        ctx.value_types[value_id] = type;
        ctx.value_roles[value_id] = role;
    };

    auto seedBufferHandle = [&](uint32_t value_id, uint32_t binding_index) {
        seedValue(value_id, "buf" + std::to_string(std::min<uint32_t>(binding_index, 30)),
                  {MSLTypeKind::DeviceCharPtr, 0, {}}, ValueRole::BufferHandle);
    };

    auto param_type_ids = functionParamTypeIds(module, fn);
    if (fn.param_count != 0 && fn.instruction_start_value >= fn.param_count) {
        uint32_t first_param_value = fn.instruction_start_value - fn.param_count;
        for (uint32_t i = 0; i < fn.param_count; i++) {
            uint32_t value_id = first_param_value + i;
            MSLType type = i < param_type_ids.size()
                ? DXILIRBuilder::resolveType(param_type_ids[i], module)
                : MSLType{MSLTypeKind::Int, 0, {}};
            if (type.kind == MSLTypeKind::Void || type.kind == MSLTypeKind::Unknown ||
                type.kind == MSLTypeKind::Struct)
                type = {MSLTypeKind::Int, 0, {}};
            if (isPointerMSLType(type)) {
                seedBufferHandle(value_id, i);
            } else {
                std::string name = emitValue(value_id);
                seedValue(value_id, name, type);
                ctx.predeclared_names.insert(name);
                ctx.predeclared_types[name] = type;
                os << "  " << typedDecl(name, type) << " = "
                   << defaultForType(type) << "; // function parameter fallback\n";
            }
        }
    }

    std::map<uint32_t, uint32_t> block_value_to_index;
    for (size_t i = 0; i < fn.block_value_ids.size(); i++)
        block_value_to_index[fn.block_value_ids[i]] = (uint32_t)i;

    std::map<uint32_t, std::set<uint32_t>> successors;
    struct TerminatorInfo { enum Kind { None, Br, Switch, Ret, Unreachable } kind = None; std::vector<uint32_t> operands; };
    std::vector<TerminatorInfo> terminators(fn.blocks.size());

    for (size_t bi = 0; bi < fn.blocks.size(); bi++) {
        auto &block = fn.blocks[bi];
        if (block.instructions.empty()) continue;
        auto &last = block.instructions.back();
        if (last.opcode == LLVMInstruction::Br) {
            terminators[bi].kind = TerminatorInfo::Br;
            terminators[bi].operands = last.operands;
            if (last.operands.size() == 1) successors[(uint32_t)bi].insert(last.operands[0]);
            else if (last.operands.size() >= 3) { successors[(uint32_t)bi].insert(last.operands[1]); successors[(uint32_t)bi].insert(last.operands[2]); }
        } else if (last.opcode == LLVMInstruction::Switch) {
            terminators[bi].kind = TerminatorInfo::Switch;
            terminators[bi].operands = last.operands;
            if (last.operands.size() >= 2) {
                successors[(uint32_t)bi].insert(last.operands[1]);
                for (size_t j = 2; j + 1 < last.operands.size(); j += 2)
                    successors[(uint32_t)bi].insert(last.operands[j + 1]);
            }
        } else if (last.opcode == LLVMInstruction::Ret) {
            terminators[bi].kind = TerminatorInfo::Ret; terminators[bi].operands = last.operands;
        } else if (last.opcode == LLVMInstruction::Unreachable) {
            terminators[bi].kind = TerminatorInfo::Unreachable;
        }
    }

    std::map<uint32_t, std::set<uint32_t>> predecessors;
    for (auto &[from, succs] : successors)
        for (uint32_t to : succs) predecessors[to].insert(from);

    struct PhiIncoming { uint32_t value_id; uint32_t pred_block_idx; };
    struct PhiInfo { uint32_t result_slot; uint32_t type_id; std::vector<PhiIncoming> incoming; };
    std::map<uint32_t, std::vector<PhiInfo>> phi_info_per_block;
    std::map<uint32_t, uint32_t> value_def_block;
    std::set<uint32_t> cross_block_values;
    std::set<uint32_t> phi_result_values;
    std::set<uint32_t> unresolved_referenced_values;
    std::map<uint32_t, MSLType> unresolved_reference_types;

    auto resultTypeForPredecl = [&](const LLVMInstruction &inst) -> MSLType {
        switch (inst.opcode) {
        case LLVMInstruction::FCmp:
        case LLVMInstruction::ICmp:
            return {MSLTypeKind::Bool, 0, {}};
        case LLVMInstruction::Alloca:
        case LLVMInstruction::GetElementPtr:
        case LLVMInstruction::IntToPtr:
            return {MSLTypeKind::DeviceCharPtr, 0, {}};
        case LLVMInstruction::BitCast: {
            MSLType declared = DXILIRBuilder::resolveType(inst.type_id, module);
            MSLType source = inst.operands.empty() || inst.operands[0] >= ctx.value_types.size()
                ? MSLType{}
                : ctx.value_types[inst.operands[0]];
            if ((typeLooksResourceHandle(source) || isPointerMSLType(source)) &&
                (DXILIRBuilder::isFloatType(declared) || DXILIRBuilder::isIntType(declared)))
                return usableType(declared) ? declared : MSLType{MSLTypeKind::Int, 0, {}};
            if (usableType(source) && !usableType(declared))
                return source;
            break;
        }
        case LLVMInstruction::Add: case LLVMInstruction::Sub: case LLVMInstruction::Mul:
        case LLVMInstruction::UDiv: case LLVMInstruction::SDiv:
        case LLVMInstruction::URem: case LLVMInstruction::SRem:
        case LLVMInstruction::And: case LLVMInstruction::Or: case LLVMInstruction::Xor:
        case LLVMInstruction::Shl: case LLVMInstruction::LShr: case LLVMInstruction::AShr:
        case LLVMInstruction::FAdd: case LLVMInstruction::FSub:
        case LLVMInstruction::FMul: case LLVMInstruction::FDiv: case LLVMInstruction::FRem: {
            MSLType declared = DXILIRBuilder::resolveType(inst.type_id, module);
            MSLType lhs = inst.operands.size() > 0 && inst.operands[0] < ctx.value_types.size()
                ? ctx.value_types[inst.operands[0]]
                : MSLType{};
            MSLType rhs = inst.operands.size() > 1 && inst.operands[1] < ctx.value_types.size()
                ? ctx.value_types[inst.operands[1]]
                : MSLType{};
            MSLType fallback = (inst.opcode == LLVMInstruction::FAdd ||
                                inst.opcode == LLVMInstruction::FSub ||
                                inst.opcode == LLVMInstruction::FMul ||
                                inst.opcode == LLVMInstruction::FDiv ||
                                inst.opcode == LLVMInstruction::FRem)
                ? MSLType{MSLTypeKind::Float, 0, {}}
                : MSLType{MSLTypeKind::Int, 0, {}};
            MSLType result = promoteNumericType(lhs, rhs, usableType(declared) ? declared : fallback);
            if (inst.opcode != LLVMInstruction::FAdd &&
                inst.opcode != LLVMInstruction::FSub &&
                inst.opcode != LLVMInstruction::FMul &&
                inst.opcode != LLVMInstruction::FDiv &&
                inst.opcode != LLVMInstruction::FRem) {
                if (result.kind == MSLTypeKind::Float2) return {MSLTypeKind::Int2, 0, {}};
                if (result.kind == MSLTypeKind::Float3) return {MSLTypeKind::Int3, 0, {}};
                if (result.kind == MSLTypeKind::Float4) return {MSLTypeKind::Int4, 0, {}};
                if (DXILIRBuilder::isFloatType(result)) return {MSLTypeKind::Int, 0, {}};
            }
            return result;
        }
        case LLVMInstruction::Call: {
            if (!inst.operands.empty()) {
                uint32_t callee = inst.operands[0];
                std::string callee_name;
                auto decl_it = ctx.function_decls.find(callee);
                if (decl_it != ctx.function_decls.end()) callee_name = decl_it->second;
                else if (callee < ctx.value_table.size()) callee_name = ctx.value_table[callee];
                uint32_t intrinsic_id = intrinsicIdFromCalleeName(callee_name);
                if (intrinsic_id != 0 && inst.operands.size() > 2) {
                    // RayQuery state operations share one LLVM declaration
                    // name.  Resolve their explicit DXIL opcode before the
                    // pre-declaration pass so a scalar state result cannot
                    // inherit the i1 type of a neighboring predicate.
                    uint32_t explicit_opcode = 0;
                    bool has_explicit_opcode = false;
                    const uint32_t opcode_id = inst.operands[2];
                    if (opcode_id < ctx.value_table.size())
                        has_explicit_opcode = parseUnsignedLiteral(
                            ctx.value_table[opcode_id], explicit_opcode);
                    for (const auto &constant : module.constants) {
                        if (!has_explicit_opcode && constant.id == opcode_id &&
                            parseUnsignedLiteral(constant.constant_data,
                                                 explicit_opcode)) {
                            has_explicit_opcode = true;
                            break;
                        }
                    }
                    if (!has_explicit_opcode) {
                        for (const auto &constant : fn.constants) {
                            if (constant.id == opcode_id &&
                                parseUnsignedLiteral(constant.constant_data,
                                                     explicit_opcode)) {
                                has_explicit_opcode = true;
                                break;
                            }
                        }
                    }
                    if (has_explicit_opcode) {
                        switch (explicit_opcode) {
                        case 193:
                        case 194:
                        case 196:
                        case 197:
                        case 205:
                        case 206:
                        case 211:
                        case 212:
                        case 198:
                        case 199:
                        case 200:
                            return {MSLTypeKind::Float, 0, {}};
                        case 190:
                        case 191:
                        case 192:
                        case 195:
                        case 201:
                        case 202:
                        case 203:
                        case 204:
                        case 207:
                        case 208:
                        case 209:
                        case 210:
                        case 214:
                        case 215:
                            return {MSLTypeKind::UInt, 0, {}};
                        default:
                            break;
                        }
                    }
                    std::vector<uint32_t> fn_args;
                    if (intrinsic_id == DXOP_Unary || intrinsic_id == DXOP_Binary ||
                        intrinsic_id == DXOP_Tertiary)
                        fn_args.assign(inst.operands.begin() + 2, inst.operands.end());
                    else
                        fn_args.assign(inst.operands.begin() + 3, inst.operands.end());
                    MSLType declared = DXILIRBuilder::resolveType(inst.type_id, module);
                    MSLType inferred = inferDXIntrinsicResultType(ctx, intrinsic_id, fn_args, declared,
                                                                  callee_name);
                    if (inferred.kind == MSLTypeKind::Void)
                        return inferred;
                    if (inferred.kind != MSLTypeKind::Unknown)
                        return inferred;
                }
            }
            break;
        }
        case LLVMInstruction::ExtractValue:
        case LLVMInstruction::ExtractElement: {
            MSLType source_type = inst.operands.empty() || inst.operands[0] >= ctx.value_types.size()
                ? MSLType{}
                : ctx.value_types[inst.operands[0]];
            if (DXILIRBuilder::isVectorType(source_type) ||
                DXILIRBuilder::isLongVectorType(source_type))
                return DXILIRBuilder::scalarType(source_type);
            break;
        }
        case LLVMInstruction::Select: {
            MSLType true_type = inst.operands.size() > 1 && inst.operands[1] < ctx.value_types.size()
                ? ctx.value_types[inst.operands[1]]
                : MSLType{};
            MSLType false_type = inst.operands.size() > 2 && inst.operands[2] < ctx.value_types.size()
                ? ctx.value_types[inst.operands[2]]
                : MSLType{};
            MSLType selected = mergePredeclType(true_type, false_type);
            if (DXILIRBuilder::isVectorType(selected))
                return selected;
            break;
        }
        case LLVMInstruction::ShuffleVector:
        case LLVMInstruction::InsertElement:
        case LLVMInstruction::InsertValue: {
            if (!inst.operands.empty() && inst.operands[0] < ctx.value_types.size() &&
                (DXILIRBuilder::isVectorType(ctx.value_types[inst.operands[0]]) ||
                 DXILIRBuilder::isLongVectorType(ctx.value_types[inst.operands[0]])))
                return ctx.value_types[inst.operands[0]];
            break;
        }
        default:
            break;
        }
        MSLType type = DXILIRBuilder::resolveType(inst.type_id, module);
        if (ctx.shader.kind != DxilShaderKind::Compute &&
            type.kind == MSLTypeKind::RWTexture2D)
            type = {MSLTypeKind::Texture2D, 0, {}};
        if (type.kind == MSLTypeKind::Unknown || type.kind == MSLTypeKind::Struct)
            type = {MSLTypeKind::Int, 0, {}};
        return type;
    };

    auto instructionProducesValue = [&](const LLVMInstruction &inst) -> bool {
        switch (inst.opcode) {
        case LLVMInstruction::Ret:
        case LLVMInstruction::Br:
        case LLVMInstruction::Switch:
        case LLVMInstruction::Unreachable:
        case LLVMInstruction::Store:
            return false;
        default:
            break;
        }

        MSLType type = resultTypeForPredecl(inst);
        return type.kind != MSLTypeKind::Void;
    };

    {
        uint32_t vc = fn.instruction_start_value;
        for (size_t bi = 0; bi < fn.blocks.size(); bi++) {
            for (auto &inst : fn.blocks[bi].instructions) {
                bool produces_value = instructionProducesValue(inst);
                if (produces_value)
                    value_def_block[vc] = (uint32_t)bi;
                if (inst.opcode == LLVMInstruction::PHI) {
                    PhiInfo pi; pi.result_slot = vc; pi.type_id = inst.type_id;
                    phi_result_values.insert(vc);
                    for (size_t j = 0; j + 1 < inst.operands.size(); j += 2) {
                        PhiIncoming inc; inc.value_id = inst.operands[j];
                        uint32_t pbv = inst.operands[j + 1];
                        auto it = block_value_to_index.find(pbv);
                        inc.pred_block_idx = it == block_value_to_index.end() ? UINT32_MAX : it->second;
                        pi.incoming.push_back(inc);
                    }
                    phi_info_per_block[(uint32_t)bi].push_back(pi);
                }
                if (produces_value)
                    vc++;
            }
        }
    }

    auto hasResolvedValue = [&](uint32_t value_id) -> bool {
        if (value_id < ctx.value_table.size() && !ctx.value_table[value_id].empty() &&
            !startsWith(ctx.value_table[value_id], "dx."))
            return true;
        if (value_def_block.find(value_id) != value_def_block.end())
            return true;
        for (auto &c : module.constants)
            if (c.id == value_id && !c.constant_data.empty())
                return true;
        for (auto &c : fn.constants)
            if (c.id == value_id && !c.constant_data.empty())
                return true;
        return false;
    };

    auto rememberUnresolvedReference = [&](uint32_t value_id, MSLType type_hint) {
        if (hasResolvedValue(value_id))
            return;
        unresolved_referenced_values.insert(value_id);
        if (type_hint.kind != MSLTypeKind::Unknown && type_hint.kind != MSLTypeKind::Void &&
            type_hint.kind != MSLTypeKind::Struct) {
            auto it = unresolved_reference_types.find(value_id);
            if (it == unresolved_reference_types.end())
                unresolved_reference_types[value_id] = type_hint;
            else
                it->second = mergePredeclType(it->second, type_hint);
        }
    };

    for (size_t bi = 0; bi < fn.blocks.size(); bi++) {
        for (auto &inst : fn.blocks[bi].instructions) {
            MSLType inst_type = DXILIRBuilder::resolveType(inst.type_id, module);
            for (size_t oi = 0; oi < inst.operands.size(); oi++) {
                bool value_operand = true;
                MSLType type_hint = inst_type;
                if (inst.opcode == LLVMInstruction::PHI)
                    value_operand = (oi % 2) == 0;
                else if (inst.opcode == LLVMInstruction::Call)
                    value_operand = oi >= 2;
                else if (inst.opcode == LLVMInstruction::Br)
                    value_operand = inst.operands.size() >= 3 && oi == 0;
                else if (inst.opcode == LLVMInstruction::Switch)
                    value_operand = oi == 0;
                else if (inst.opcode == LLVMInstruction::AtomicRMW)
                    value_operand = oi < 2;
                else if (inst.opcode == LLVMInstruction::CmpXchg)
                    value_operand = oi < 3;
                if (!value_operand) continue;

                if ((inst.opcode == LLVMInstruction::Load ||
                     inst.opcode == LLVMInstruction::Store ||
                     inst.opcode == LLVMInstruction::GetElementPtr ||
                     inst.opcode == LLVMInstruction::GEP ||
                     inst.opcode == LLVMInstruction::AtomicRMW ||
                     inst.opcode == LLVMInstruction::CmpXchg) && oi == 0)
                    type_hint = {MSLTypeKind::DeviceCharPtr, 0, {}};
                else if (inst.opcode == LLVMInstruction::PHI)
                    type_hint = inst_type;
                rememberUnresolvedReference(inst.operands[oi], type_hint);

                auto def_it = value_def_block.find(inst.operands[oi]);
                if (def_it != value_def_block.end() && def_it->second != (uint32_t)bi)
                    cross_block_values.insert(inst.operands[oi]);
            }
        }
    }

    uint32_t value_counter = fn.instruction_start_value;
    ctx.instruction_start_value = fn.instruction_start_value;

    bool needs_dispatch = fn.blocks.size() > 1;
    if (needs_dispatch) {
        uint32_t vc = fn.instruction_start_value;
        for (auto &block : fn.blocks) {
            for (auto &inst : block.instructions) {
                bool produces_value = instructionProducesValue(inst);
                if (produces_value && inst.opcode == LLVMInstruction::Alloca) {
                    std::string storage_class = "thread";
                    if (inst.type_id > 0 && inst.type_id < module.types.size()) {
                        auto &ptr_type = module.types[inst.type_id];
                        if (ptr_type.kind == LLVMType::Pointer && ptr_type.address_space == 3)
                            storage_class = "threadgroup";
                    }
                    std::string alloca_name = "alloca_" + std::to_string(vc);
                    os << "  " << storage_class << " char " << alloca_name
                       << "[256]; // dispatch alloca storage\n";
                    if (ctx.value_table.size() <= vc) ctx.value_table.resize(vc + 1);
                    if (ctx.value_types.size() <= vc) ctx.value_types.resize(vc + 1);
                    ctx.value_table[vc] = "(" + storage_class + " char*)&" + alloca_name;
                    ctx.value_types[vc] = storage_class == "threadgroup"
                        ? MSLType{MSLTypeKind::ThreadgroupCharPtr, 0, {}}
                        : MSLType{MSLTypeKind::DeviceCharPtr, 0, {}};
                    ctx.predeclared_allocas.insert(vc);
                }
                if (produces_value) vc++;
            }
        }
    }

    for (uint32_t value_id : unresolved_referenced_values) {
        auto type_it = unresolved_reference_types.find(value_id);
        if (value_id < 31 && type_it != unresolved_reference_types.end() &&
            isPointerMSLType(type_it->second) && !hasResolvedValue(value_id))
            seedBufferHandle(value_id, value_id);
    }

    for (uint32_t value_id : unresolved_referenced_values) {
        if (hasResolvedValue(value_id))
            continue;
        MSLType pre_type = {MSLTypeKind::Int, 0, {}};
        auto type_it = unresolved_reference_types.find(value_id);
        if (type_it != unresolved_reference_types.end())
            pre_type = type_it->second;
        std::string name = emitValue(value_id);
        bool declaration_slot = ctx.function_decls.find(value_id) != ctx.function_decls.end();
        if (ctx.value_table.size() <= value_id) ctx.value_table.resize(value_id + 1);
        if (ctx.value_types.size() <= value_id) ctx.value_types.resize(value_id + 1);
        if (!declaration_slot)
            ctx.value_table[value_id] = name;
        ctx.value_types[value_id] = pre_type;
        ctx.predeclared_names.insert(name);
        ctx.predeclared_types[name] = pre_type;
        os << "  " << typedDecl(name, pre_type) << " = "
           << defaultForType(pre_type) << "; // unresolved value pre-decl\n";
    }

    if (needs_dispatch) {
        uint32_t vc = fn.instruction_start_value;
        for (size_t bi = 0; bi < fn.blocks.size(); bi++) {
            for (auto &inst : fn.blocks[bi].instructions) {
                bool produces_value = instructionProducesValue(inst);
                MSLType static_type;
                if (produces_value) {
                    if (ctx.value_types.size() <= vc) ctx.value_types.resize(vc + 1);
                    static_type = mergePredeclType(ctx.value_types[vc], resultTypeForPredecl(inst));
                    ctx.value_types[vc] = static_type;
                }
                if (produces_value) {
                    MSLType pre_type = static_type;
                    if (ctx.shader.kind != DxilShaderKind::Compute &&
                        pre_type.kind == MSLTypeKind::RWTexture2D)
                        pre_type = {MSLTypeKind::Texture2D, 0, {}};
                    std::string name = emitValue(vc);
                    if (ctx.value_table.size() <= vc) ctx.value_table.resize(vc + 1);
                    ctx.value_table[vc] = name;
                    // Resource handles are materialized directly as ABI
                    // arguments by the intrinsic emitter.  Declaring a
                    // temporary texture here can give an SRV handle a
                    // read_write access qualifier (or vice versa) before
                    // AnnotateHandle has supplied its resource metadata.
                    if (!typeLooksResourceHandle(pre_type)) {
                        ctx.predeclared_names.insert(name);
                        ctx.predeclared_types[name] = pre_type;
                        os << "  " << typedDecl(name, pre_type) << " = "
                           << defaultForType(pre_type) << "; // dispatch pre-decl\n";
                    }
                }
                if (produces_value) vc++;
            }
        }
    }
    if (needs_dispatch) {
        os << "  int _block_state = 0;\n  for (;;) {\n    switch (_block_state) {\n";
    }

    for (size_t bi = 0; bi < fn.blocks.size(); bi++) {
        auto &block = fn.blocks[bi];
        if (needs_dispatch) os << "    case " << bi << ": {\n";

        for (size_t ii = 0; ii < block.instructions.size(); ii++) {
            auto &inst = block.instructions[ii];
            bool is_terminator = (inst.opcode == LLVMInstruction::Br ||
                                   inst.opcode == LLVMInstruction::Switch ||
                                   inst.opcode == LLVMInstruction::Ret ||
                                   inst.opcode == LLVMInstruction::Unreachable);

            if (inst.opcode == LLVMInstruction::PHI) {
                ctx.value_table.resize(std::max((size_t)value_counter + 1, ctx.value_table.size()));
                ctx.value_types.resize(std::max((size_t)value_counter + 1, ctx.value_types.size()));
                ctx.value_table[value_counter] = emitValue(value_counter);
                ctx.value_types[value_counter] = DXILIRBuilder::resolveType(inst.type_id, module);
                value_counter++;
                continue;
            }

            if (!is_terminator) {
                emitTypedInstruction(ctx, inst, value_counter);
                continue;
            }

            auto succ_it = successors.find((uint32_t)bi);
            if (succ_it != successors.end()) {
                for (uint32_t succ : succ_it->second) {
                    auto phi_it = phi_info_per_block.find(succ);
                    if (phi_it != phi_info_per_block.end()) {
                        for (auto &pi : phi_it->second) {
                            for (auto &inc : pi.incoming) {
                                if (inc.pred_block_idx == (uint32_t)bi) {
                                    MSLType phi_type = pi.result_slot < ctx.value_types.size()
                                        ? ctx.value_types[pi.result_slot]
                                        : DXILIRBuilder::resolveType(pi.type_id, module);
                                    auto phi_pre_it = ctx.predeclared_types.find(emitValue(pi.result_slot));
                                    if (phi_pre_it != ctx.predeclared_types.end())
                                        phi_type = phi_pre_it->second;
                                    std::string incoming = hasEmittableValue(ctx, inc.value_id)
                                        ? resolveValue(ctx, inc.value_id)
                                        : defaultForType(phi_type);
                                    os << "    " << emitValue(pi.result_slot) << " = "
                                       << coerceResolvedValue(ctx, incoming, phi_type) << ";\n";
                                }
                            }
                        }
                    }
                }
            }

            if (inst.opcode == LLVMInstruction::Ret) {
                if (ctx.shader.kind == DxilShaderKind::Vertex) os << "    return out;\n";
                else if (ctx.shader.kind == DxilShaderKind::Pixel) {
                    if (!inst.operands.empty()) os << "    result.color0 = float4(" << resolveValue(ctx, inst.operands[0]) << ");\n";
                    os << "    return result;\n";
                } else {
                    if (!inst.operands.empty()) os << "    return " << resolveValue(ctx, inst.operands[0]) << ";\n";
                    else os << "    return;\n";
                }
            } else if (inst.opcode == LLVMInstruction::Br) {
                if (inst.operands.size() == 1 && needs_dispatch) {
                    os << "    _block_state = " << inst.operands[0] << "; continue;\n";
                } else if (inst.operands.size() >= 3 && needs_dispatch) {
                    os << "    if (" << resolveCondition(ctx, inst.operands[0]) << ") { _block_state = " << inst.operands[1] << "; continue; } else { _block_state = " << inst.operands[2] << "; continue; }\n";
                }
            } else if (inst.opcode == LLVMInstruction::Switch) {
                if (inst.operands.size() >= 2) {
                    os << "    { int _sv = (int)(" << coerceResolvedValue(ctx, resolveValue(ctx, inst.operands[0]), {MSLTypeKind::Int, 0, {}}) << ");\n";
                    for (size_t j = 2; j + 1 < inst.operands.size(); j += 2)
                        os << "    if (_sv == " << inst.operands[j] << ") { _block_state = " << inst.operands[j+1] << "; continue; }\n";
                    os << "    _block_state = " << inst.operands[1] << "; continue; }\n";
                }
            } else if (inst.opcode == LLVMInstruction::Unreachable) {
                os << "    // unreachable\n";
            }

            if (needs_dispatch) os << "    }\n";
        }
    }

    if (needs_dispatch) os << "    }\n    break;\n  }\n";
    os << "}\n";

    TypedMSLShader result;
    result.source = hardenGeneratedBoolVectorAssignments(os.str());
    result.entry_point = shader.entry_point;
    result.tg_size[0] = module.num_threads[0];
    result.tg_size[1] = module.num_threads[1];
    result.tg_size[2] = module.num_threads[2];
    result.unsupported_intrinsics = ctx.unsupported_intrinsics;
    result.unsupported_opcodes = ctx.unsupported_opcodes;
    result.diagnostics = ctx.diagnostics;

    return std::optional<TypedMSLShader>(std::in_place, std::move(result));
}

}
