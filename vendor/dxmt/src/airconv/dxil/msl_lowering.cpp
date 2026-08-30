#include "msl_lowering.hpp"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <algorithm>
#include <map>
#include <optional>
#include <set>

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
  DXOP_TextureGather = 73,
  DXOP_TextureSample = 60,
  DXOP_TextureSampleBias = 61,
  DXOP_TextureSampleLevel = 62,
  DXOP_TextureSampleGrad = 63,
  DXOP_TextureSampleCmp = 64,
  DXOP_TextureSampleCmpLevelZero = 65,
  DXOP_Barrier = 80,
  DXOP_Unary = 13,
  DXOP_Binary = 14,
  DXOP_Tertiary = 15,
  DXOP_Dot2 = 54,
  DXOP_Dot3 = 55,
  DXOP_Dot4 = 56,
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
  DXOP_LegacyF16ToF32 = 131,
  DXOP_LegacyF32ToF16 = 132,
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
  DXOP_QuadReadLaneAt = 122,
  DXOP_QuadOp = 123,
  DXOP_QuadVote = 222,
  DXOP_TextureStoreSample = 225,
  DXOP_TextureSampleCmpLevel = 224,
  DXOP_TextureGatherCmp = 74,
  DXOP_TextureGatherRaw = 223,
  DXOP_WriteSamplerFeedback = 174,
  DXOP_WriteSamplerFeedbackBias = 175,
  DXOP_WriteSamplerFeedbackLevel = 176,
  DXOP_WriteSamplerFeedbackGrad = 177,
  // dx.op.isSpecialFloat carries the concrete operation (8..11) in its
  // opcode argument rather than in the intrinsic name.
  DXOP_SpecialFloat = 1000,
};

enum DXILMathOpcode {
  DXILOP_FAbs = 6, DXILOP_Saturate = 7, DXILOP_IsNaN = 8, DXILOP_IsInf = 9,
  DXILOP_IsFinite = 10, DXILOP_Cos = 12, DXILOP_Sin = 13, DXILOP_Tan = 14,
  DXILOP_Acos = 15, DXILOP_Asin = 16, DXILOP_Atan = 17,
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
  DXILOP_Ibfe = 51, DXILOP_Ubfe = 52, DXILOP_Bfi = 53,
};

static const char *kMetalHeader = R"(#include <metal_stdlib>
using namespace metal;

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
    if (strncmp(s, "sampleCmpLevel.", 15) == 0) return 224;
    if (strncmp(s, "sampleCmp.", 10) == 0) return 64;
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
    if (strncmp(s, "barrier", 7) == 0) return 80;
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
    if (strncmp(s, "makeDouble", 10) == 0) return 101;
    if (strncmp(s, "splitDouble", 11) == 0) return 102;
    if (strncmp(s, "legacyF16ToF32", 14) == 0) return 131;
    if (strncmp(s, "legacyF32ToF16", 14) == 0) return 132;
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
    if (strncmp(s, "wavePrefixOp", 12) == 0) return 121;
    if (strncmp(s, "quadReadLaneAt", 14) == 0) return 122;
    if (strncmp(s, "quadOp", 6) == 0) return 123;
    if (strncmp(s, "quadVote", 8) == 0) return 222;
    if (strncmp(s, "writeSamplerFeedbackLevel", 25) == 0) return 176;
    if (strncmp(s, "writeSamplerFeedbackGrad", 24) == 0) return 177;
    if (strncmp(s, "writeSamplerFeedbackBias", 24) == 0) return 175;
    if (strncmp(s, "writeSamplerFeedback", 20) == 0) return 174;
    if (strncmp(s, "isSpecialFloat.", 15) == 0) return DXOP_SpecialFloat;
    if (strncmp(s, "cycleCounterLegacy", 18) == 0) return 109;
    if (strncmp(s, "texture2DMSGetSamplePosition", 27) == 0) return 75;
    if (strncmp(s, "renderTargetGetSamplePosition", 29) == 0) return 76;
    if (strncmp(s, "renderTargetGetSampleCount", 26) == 0) return 77;
    return 0;
}

static bool isOpcodePrefixedDXIntrinsic(uint32_t opcode) {
    switch (opcode) {
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
    case DXOP_TextureLoad:
    case DXOP_TextureStore:
    case DXOP_TextureGather:
    case DXOP_TextureStoreSample:
    case DXOP_BufferLoad:
    case DXOP_BufferStore:
    case DXOP_RawBufferLoad:
    case DXOP_RawBufferStore:
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
    case DXOP_WavePrefixOp:
    case DXOP_QuadReadLaneAt:
    case DXOP_QuadOp:
    case DXOP_QuadVote:
    case DXOP_WriteSamplerFeedback:
    case DXOP_WriteSamplerFeedbackBias:
    case DXOP_WriteSamplerFeedbackLevel:
    case DXOP_WriteSamplerFeedbackGrad:
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
    if (!DXILIRBuilder::isVectorType(type))
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

    for (size_t i = 0; i < parts.size(); i++) {
        if (i) args += ", ";
        args += DXILIRBuilder::isVectorType(type) ? scalarize_vector_part(parts[i]) : parts[i];
    }
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
};

struct ResourceHandleRecord {
    DescriptorRangePlan::Kind kind = DescriptorRangePlan::Kind::SRV;
    uint32_t resource_class = 0;
    uint32_t register_space = 0;
    uint32_t lower_bound = 0;
    uint32_t binding_index = 0;
    uint32_t resource_kind = 0;
    uint32_t element_stride = 0;
    uint32_t sample_count = 1;
    bool non_uniform = false;
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
    bool compute_sample_cmp_shader = false;
    bool compute_texture_sample_shader = false;
    bool uses_atomic64_emulation = false;
    bool uses_group_atomic64_emulation = false;
    bool uses_sampler_feedback = false;
};

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
           << " count=" << range.count << "\n";
    }
    os << "\n";
}

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
    os << "  float4 v0 [[user(locn0)]]; float4 v1 [[user(locn1)]];\n";
    os << "  float4 v2 [[user(locn2)]]; float4 v3 [[user(locn3)]];\n";
    os << "  float4 v4 [[user(locn4)]]; float4 v5 [[user(locn5)]];\n";
    os << "  float4 v6 [[user(locn6)]]; float4 v7 [[user(locn7)]];\n";
    os << "  float2 uv0 [[user(locn8)]]; float2 uv1 [[user(locn9)]];\n";
    os << "  float2 uv2 [[user(locn10)]]; float2 uv3 [[user(locn11)]];\n";
    os << "  float4 color0 [[user(locn12)]]; float4 color1 [[user(locn13)]];\n";
    os << "  float4 color2 [[user(locn14)]]; float4 color3 [[user(locn15)]];\n";
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
        for (uint32_t i = 0; i < ctx.binding_plan.direct_buffer_count; i++)
            os << "  device char* buf" << i << " [[buffer(" << i << ")]],\n";
        for (uint32_t i = 0; i < ctx.binding_plan.direct_texture_count; i++) {
            bool comparison_slot = false;
            bool raw_gather_slot = false;
            bool srv_slot = false;
            bool uav_slot = false;
            if (ctx.compute_sample_cmp_shader) {
                for (const auto &range : ctx.binding_plan.ranges) {
                    if (range.kind == DescriptorRangePlan::Kind::SRV &&
                        i >= range.lower_bound &&
                        i < range.lower_bound + range.count) {
                        comparison_slot = true;
                        break;
                    }
                }
            }
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
            if (raw_gather_slot)
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
        for (uint32_t i = 0; i < ctx.binding_plan.direct_buffer_count; i++)
            params.push_back("  device char* buf" + std::to_string(i) +
                             " [[buffer(" + std::to_string(i) + ")]]");
        for (uint32_t i = 0; i < ctx.binding_plan.direct_texture_count; i++)
            params.push_back("  texture2d<float, access::sample> tex" + std::to_string(i) +
                             " [[texture(" + std::to_string(i) + ")]]");
        for (uint32_t i = 0; i < ctx.binding_plan.direct_sampler_count; i++)
            params.push_back("  sampler samp" + std::to_string(i) +
                             " [[sampler(" + std::to_string(i) + ")]]");
        for (size_t i = 0; i < params.size(); i++)
            os << params[i] << (i + 1 == params.size() ? "\n" : ",\n");
        os << ") {\n";
        os << "  output_v out = {};\n";
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
            os << "static inline bool m12_cons_point_in_box(float2 p, float2 lo, float2 hi) { return p.x >= lo.x-1.0e-5f && p.x <= hi.x+1.0e-5f && p.y >= lo.y-1.0e-5f && p.y <= hi.y+1.0e-5f; }\n";
            os << "static inline bool m12_cons_seg_intersects(float2 a, float2 b, float2 c, float2 d) { float e=1.0e-5f; float ab_c=m12_cons_cross(a,b,c), ab_d=m12_cons_cross(a,b,d), cd_a=m12_cons_cross(c,d,a), cd_b=m12_cons_cross(c,d,b); return ((ab_c >= -e && ab_d <= e) || (ab_c <= e && ab_d >= -e)) && ((cd_a >= -e && cd_b <= e) || (cd_a <= e && cd_b >= -e)); }\n";
            os << "static inline bool m12_cons_triangle_pixel(float2 a, float2 b, float2 c, float2 lo) { float2 hi=lo+1.0f; float2 q0=float2(lo.x,lo.y), q1=float2(hi.x,lo.y), q2=float2(hi.x,hi.y), q3=float2(lo.x,hi.y); if (m12_cons_point_in_box(a,lo,hi) || m12_cons_point_in_box(b,lo,hi) || m12_cons_point_in_box(c,lo,hi)) return true; if (m12_cons_point_in_tri(q0,a,b,c) || m12_cons_point_in_tri(q1,a,b,c) || m12_cons_point_in_tri(q2,a,b,c) || m12_cons_point_in_tri(q3,a,b,c)) return true; return m12_cons_seg_intersects(a,b,q0,q1) || m12_cons_seg_intersects(a,b,q1,q2) || m12_cons_seg_intersects(a,b,q2,q3) || m12_cons_seg_intersects(a,b,q3,q0) || m12_cons_seg_intersects(b,c,q0,q1) || m12_cons_seg_intersects(b,c,q1,q2) || m12_cons_seg_intersects(b,c,q2,q3) || m12_cons_seg_intersects(b,c,q3,q0) || m12_cons_seg_intersects(c,a,q0,q1) || m12_cons_seg_intersects(c,a,q1,q2) || m12_cons_seg_intersects(c,a,q2,q3) || m12_cons_seg_intersects(c,a,q3,q0); }\n";
        }
        if (ctx.options.depth_bounds_test)
            ctx.binding_plan.direct_buffer_count =
                std::min<uint32_t>(ctx.binding_plan.direct_buffer_count, 28);
        os << "fragment float4 ps_main(\n";
        os << "  input_v in [[stage_in]],\n";
        for (uint32_t i = 0; i < ctx.binding_plan.direct_buffer_count; i++)
            os << "  device char* buf" << i << " [[buffer(" << i << ")]],\n";
        if (ctx.options.conservative_rasterization)
            os << "  constant m12_conservative_data& m12_conservative [[buffer(26)]],\n";
        for (uint32_t i = 0; i < ctx.binding_plan.direct_texture_count; i++) {
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
            if (uav_slot && ctx.uses_sampler_feedback && srv_slot)
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
    return startsWith(value, "tex") || startsWith(value, "samp") || startsWith(value, "buf");
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

    if (value.find("reinterpret_cast<device float4&>") != std::string::npos ||
        value.find(".read(") != std::string::npos ||
        value.find(".sample(") != std::string::npos ||
        value.find(".gather(") != std::string::npos)
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
        stripped.find(".gather(") != std::string::npos)
        return "float4(0.0f)";
    if (stripped.find("reinterpret_cast<device uint4&>") != std::string::npos)
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
           value.find(".gather(") != std::string::npos;
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
        typeLooksResourceHandle(source_type))
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
    if (handle.kind == DescriptorRangePlan::Kind::UAV &&
        (handle.resource_kind == 3u || handle.resource_kind == 8u))
        return {MSLTypeKind::RWTexture2DArray, 0, {}};
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
    // D3D12 keeps SRV and UAV/CBV register namespaces independent.  The
    // direct MSL ABI has one buffer namespace, so place SRV buffers in the
    // upper half of the 31-slot direct buffer range.  Textures retain their
    // texture namespace and are unaffected.
    if (std::strcmp(target_prefix, "buf") == 0 &&
        handle.kind == DescriptorRangePlan::Kind::SRV)
        binding_index += 16;
    return binding_index;
}

static uint32_t cappedBindingIndex(const LowerContext &ctx, const char *prefix, uint32_t binding_index) {
    uint32_t limit = 0;
    if (std::strcmp(prefix, "buf") == 0)
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
    const char *prefix = target_prefix ? target_prefix : bindingPrefixForKind(handle.kind);
    uint32_t binding_index = target_prefix &&
                                     std::strcmp(target_prefix, "buf") == 0
                                 ? directBufferBindingIndex(ctx, handle, target_prefix)
                                 : handle.lower_bound + handle.binding_index;
    return std::string(prefix) + std::to_string(cappedBindingIndex(ctx, prefix, binding_index));
}

static void recordDescriptorRange(BindingPlan &plan, DescriptorRangePlan range) {
    if (range.count == 0)
        range.count = 1;
    for (auto &existing : plan.ranges) {
        if (existing.kind == range.kind &&
            existing.register_space == range.register_space &&
            existing.lower_bound == range.lower_bound) {
            existing.count = std::max(existing.count, range.count);
            return;
        }
    }
    plan.ranges.push_back(range);
}

static void analyzeBindingPlan(LowerContext &ctx, const LLVMFunction &fn) {
    BindingPlan plan;
    struct HandleBinding {
        DescriptorRangePlan::Kind kind = DescriptorRangePlan::Kind::SRV;
        uint32_t lower_bound = 0;
        uint32_t count = 1;
    };
    std::unordered_map<uint32_t, HandleBinding> handle_bindings;
    auto rememberHandle = [&](uint32_t result_id,
                              DescriptorRangePlan::Kind kind,
                              uint32_t lower_bound, uint32_t count,
                              uint32_t binding_index = 0) {
        if (!result_id)
            return;
        handle_bindings[result_id] = {kind, lower_bound, count};
        ResourceHandleRecord record;
        record.kind = kind;
        record.lower_bound = lower_bound;
        record.binding_index = binding_index;
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
            if (inst.opcode != LLVMInstruction::Call || inst.operands.size() < 2) {
                if (producesValue(inst))
                    ++value_counter;
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
                    intrinsic_id = opcode;
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
                uint32_t index = literalFromValue(ctx, fn_args[2], 0);
                uint32_t non_uniform_raw = fn_args.size() >= 4 ? literalFromValue(ctx, fn_args[3], 0) : 0;
                if (non_uniform_raw > 1)
                    index = 0;
                auto kind = descriptorKindForResourceClass(resource_class);
                recordDescriptorRange(plan, {kind, 0, index, 1});
                rememberHandle(result_id, kind, 0, 1, index);
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
                recordDescriptorRange(plan, {kind, space, lower_bound, count});
                rememberHandle(result_id, kind, lower_bound, count);
            } else if (intrinsic_id == DXOP_CreateHandleFromHeap && fn_args.size() >= 1) {
                uint32_t heap_index = literalFromValue(ctx, fn_args[0], 0);
                bool sampler = fn_args.size() >= 2 && literalFromValue(ctx, fn_args[1], 0) != 0;
                auto kind = sampler ? DescriptorRangePlan::Kind::Sampler
                                     : DescriptorRangePlan::Kind::SRV;
                recordDescriptorRange(plan, {kind, 0, heap_index, 1});
                rememberHandle(result_id, kind, heap_index, 1);
            } else if (intrinsic_id == DXOP_AnnotateHandle &&
                       fn_args.size() >= 2) {
                auto base = handle_bindings.find(fn_args[0]);
                if (base != handle_bindings.end()) {
                    rememberHandle(result_id, base->second.kind,
                                   base->second.lower_bound,
                                   base->second.count);
                    auto properties = parseAggregateLiteral(
                        resolveValue(ctx, fn_args[1]));
                    auto base_record = ctx.resource_handles.find(fn_args[0]);
                    if (base_record != ctx.resource_handles.end()) {
                        auto annotated = base_record->second;
                        if (!properties.empty())
                            parseUnsignedLiteral(properties[0],
                                                 annotated.resource_kind);
                        if (properties.size() > 1)
                            parseUnsignedLiteral(properties[1],
                                                 annotated.element_stride);
                        ctx.resource_handles[result_id] = annotated;
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
            if (producesValue(inst))
                ++value_counter;
        }
    }

    uint32_t max_sampler = 0;
    uint32_t max_texture = 0;
    bool has_sampler = false;
    bool has_texture = false;
    for (const auto &range : plan.ranges) {
        if (range.kind == DescriptorRangePlan::Kind::Sampler) {
            has_sampler = true;
            max_sampler = std::max(max_sampler, range.lower_bound + range.count);
        } else if (range.kind == DescriptorRangePlan::Kind::SRV ||
                   range.kind == DescriptorRangePlan::Kind::UAV) {
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
    case 225:
    case 1026:
        return {MSLTypeKind::Void, 0, {}};
    case DXOP_CBufferLoad:
    case DXOP_CBufferLoadLegacy:
    case DXOP_TextureLoad:
    case DXOP_TextureSample:
    case DXOP_TextureSampleBias:
    case DXOP_TextureSampleLevel:
    case DXOP_TextureSampleGrad:
    case DXOP_TextureGather:
        return {MSLTypeKind::Float4, 0, {}};
    case DXOP_BufferLoad:
        return callee_name.find(".i32") != std::string::npos
                   ? MSLType{MSLTypeKind::UInt4, 0, {}}
                   : MSLType{MSLTypeKind::Float4, 0, {}};
    case DXOP_TextureGatherCmp:
        return ctx.shader.kind == DxilShaderKind::Compute
                   ? MSLType{MSLTypeKind::Float, 0, {}}
                   : MSLType{MSLTypeKind::Float4, 0, {}};
    case DXOP_TextureGatherRaw:
        return {MSLTypeKind::UInt4, 0, {}};
    case DXOP_RawBufferLoad:
    case 303:
    case 1025:
    case DXOP_GetDimensions:
        return {MSLTypeKind::UInt4, 0, {}};
    case DXOP_TextureSampleCmp:
    case DXOP_TextureSampleCmpLevelZero:
    case DXOP_TextureSampleCmpLevel:
    case DXOP_CalcLOD:
    case DXOP_Dot2:
    case DXOP_Dot3:
    case DXOP_Dot4:
    case DXOP_LegacyF16ToF32:
        return {MSLTypeKind::Float, 0, {}};
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
        return {MSLTypeKind::Bool, 0, {}};
    case DXOP_ThreadId:
    case DXOP_GroupId:
    case DXOP_ThreadIDInGroup:
    case DXOP_FlattenedThreadIDInGroup:
    case DXOP_BufferUpdateCounter:
    case DXOP_AtomicBinOp:
    case DXOP_AtomicCompareExchange:
        if (callee_name.find(".i64") != std::string::npos)
            return {MSLTypeKind::Long, 0, {}};
        return {MSLTypeKind::UInt, 0, {}};
    case DXOP_WaveGetLaneIndex:
    case DXOP_WaveGetLaneCount:
    case DXOP_LegacyF32ToF16:
        return {MSLTypeKind::UInt, 0, {}};
    case DXOP_DerivCoarseX:
    case DXOP_DerivCoarseY:
    case DXOP_DerivFineX:
    case DXOP_DerivFineY:
        return args.empty() ? MSLType{MSLTypeKind::Float, 0, {}} : valueTypeOrUnknown(ctx, args[0]);
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
        MSLType promoted = promoteNumericType(a, b, {MSLTypeKind::Int, 0, {}});
        if (op == DXILOP_UMax || op == DXILOP_UMin || op == DXILOP_UMul || op == DXILOP_UDiv ||
            op == DXILOP_UAddc || op == DXILOP_USubb)
            return DXILIRBuilder::isVectorType(promoted)
                ? DXILIRBuilder::vectorOfType({MSLTypeKind::UInt, 0, {}}, DXILIRBuilder::vectorWidth(promoted))
                : MSLType{MSLTypeKind::UInt, 0, {}};
        return promoted;
    }
    case DXOP_Tertiary: {
        MSLType a = args.size() > 1 ? valueTypeOrUnknown(ctx, args[1]) : MSLType{};
        MSLType b = args.size() > 2 ? valueTypeOrUnknown(ctx, args[2]) : MSLType{};
        MSLType c = args.size() > 3 ? valueTypeOrUnknown(ctx, args[3]) : MSLType{};
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
    for (auto *prefix : {"tex", "buf", "samp"}) {
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
            (std::strcmp(prefix, "samp") == 0 &&
             type.kind == MSLTypeKind::Sampler))
            return resolveBindingName(value, prefix);
        return fallback;
    };

    auto recordHandle = [&](DescriptorRangePlan::Kind kind, uint32_t resource_class,
                            uint32_t lower_bound, uint32_t binding_index,
                            uint32_t register_space = 0, bool non_uniform = false) -> std::string {
        ResourceHandleRecord handle;
        handle.kind = kind;
        handle.resource_class = resource_class;
        handle.register_space = register_space;
        handle.lower_bound = lower_bound;
        handle.binding_index = binding_index;
        handle.non_uniform = non_uniform;
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
        (void)range_id;
        return recordHandle(descriptorKindForResourceClass(resource_class),
                            resource_class, 0, index, 0, non_uniform);
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
                        parseUnsignedLiteral(properties[1],
                                             annotated.element_stride);
                        if (annotated.resource_kind == 3u ||
                            annotated.resource_kind == 8u) {
                            uint32_t encoded_samples =
                                annotated.element_stride >> 16;
                            annotated.sample_count =
                                encoded_samples ? encoded_samples : 1u;
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
        return recordHandle(descriptorKindForResourceClass(resource_class),
                            resource_class, lower_bound, index, register_space, non_uniform);
    }
    case DXOP_CreateHandleFromHeap: {
        uint32_t heap_index = literalArg(0, 0, "heap");
        bool sampler = args.size() >= 2 && literalArg(1, 0, "samp") != 0;
        return recordHandle(sampler ? DescriptorRangePlan::Kind::Sampler
                                    : DescriptorRangePlan::Kind::SRV,
                            sampler ? 3 : 0, heap_index, 0);
    }
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
        if (!startsWith(handle, "buf"))
            return "float4(0)";
        ctx.last_buffer_handle = handle;
        auto reg = ensureScalarIndex(numericArg(1, "0"));
        return "(reinterpret_cast<device float4&>(" + handle + "[((int)(" + reg + "))*64]))";
    }
    case DXOP_BufferLoad: {
        if (args.size() < 3) return "float4(0)";
        auto handle = handleArg(0, "buf", "buf0");
        if (!startsWith(handle, "buf"))
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
        if (callee_name.find(".i32") != std::string::npos)
            return "uint4(reinterpret_cast<device uint&>(" + handle + "[" +
                   byte_offset + "]), 0u, 0u, 0u)";
        return "(reinterpret_cast<device float4&>(" + handle + "[" +
               byte_offset + "]))";
    }
    case DXOP_RawBufferLoad: case 303: case 1025: {
        if (args.size() < 3) return "uint4(0)";
        auto handle = handleArg(0, "buf", "buf0");
        if (!startsWith(handle, "buf"))
            return "uint4(0)";
        ctx.last_buffer_handle = handle;
        auto idx = ensureScalarIndex(numericArg(1, "0"));
        auto off = ensureScalarIndex(numericArg(2, "0"));
        return "(reinterpret_cast<device uint4&>(" + handle + "[(((int)(" + idx + "))*4 + ((int)(" + off + ")))]))";
    }
    case DXOP_BufferStore: case DXOP_RawBufferStore: case 1026: {
        if (args.size() < 4) return "";
        auto handle = handleArg(0, "buf", "buf0");
        if (!startsWith(handle, "buf"))
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
        auto val = numericArg(3, "0");
        std::string base = "(((int)(" + idx + "))*4 + ((int)(" + off + ")))";
        std::ostringstream store;
        for (uint32_t i = 0; i < 4; i++) {
            if (i) store << ";\n  ";
            store << "reinterpret_cast<device uint&>(" << handle << "[(" << base << ") + " << (i*4)
                  << "]) = (uint)(" << val << ")";
        }
        return store.str();
    }
    case DXOP_TextureLoad: {
        if (args.size() < 3) return "float4(0)";
        auto handle = handleArg(0, "tex", "tex0");
        ctx.last_buffer_handle = handle;
        auto cx = textureCoordComponent(ctx, valueArg(2, "0"), 0);
        auto cy = textureCoordComponent(ctx, valueArg(3, "0"), 1);
        if (ctx.compute_sample_cmp_shader) {
            auto mip = ensureScalarIndex(numericArg(1, "0"));
            return "float4(" + handle + ".read(uint2(" + cx + ", " + cy +
                   "), (uint)(" + mip + ")))";
        }
        if (isWritableMSAAHandle(ctx, args[0])) {
            auto sample = ensureScalarIndex(numericArg(1, "0"));
            auto array_slice = isWritableMSAAArrayHandle(ctx, args[0])
                                   ? ensureScalarIndex(numericArg(4, "0"))
                                   : "0";
            return handle + ".read(uint2(" + cx + ", " + cy +
                   "), " + writableMSAASlice(ctx, args[0], sample,
                                               array_slice) + ")";
        }
        return handle + ".read(uint2(" + cx + ", " + cy + "))";
    }
    case DXOP_TextureStore: case 225: {
        if (args.size() < 6) return "";
        auto handle = handleArg(0, "tex", "tex0");
        auto cx = ensureScalarIndex(numericArg(1, "0"));
        auto cy = ensureScalarIndex(numericArg(2, "0"));
        size_t vb = 4;
        std::string value = "float4(" + numericArg(vb, "0.0") + ", " + numericArg(vb+1, "0.0") +
                            ", " + numericArg(vb+2, "0.0") + ", " + numericArg(vb+3, "0.0") + ")";
        if (startsWith(handle, "buf"))
            return "reinterpret_cast<device float4&>(" + handle + "[(((int)(" + cy + "))*4096 + ((int)(" + cx + "))*16)]) = " + value;
        if (intrinsic_id == DXOP_TextureStoreSample &&
            isWritableMSAAHandle(ctx, args[0])) {
            auto sample = ensureScalarIndex(numericArg(9, "0"));
            auto array_slice = isWritableMSAAArrayHandle(ctx, args[0])
                                   ? ensureScalarIndex(numericArg(3, "0"))
                                   : "0";
            return handle + ".write(" + value + ", uint2((uint)(" + cx + "), (uint)(" + cy + ")), " +
                   writableMSAASlice(ctx, args[0], sample, array_slice) + ")";
        }
        return handle + ".write(" + value + ", uint2((uint)(" + cx + "), (uint)(" + cy + ")))";
    }
    case DXOP_TextureSample: case DXOP_TextureSampleBias:
    case DXOP_TextureSampleLevel: case DXOP_TextureSampleGrad: {
        if (args.size() < 4) return "float4(0)";
        auto handle = handleArg(0, "tex", "tex0");
        auto samp = handleArg(1, "samp", "samp0");
        auto cx = sampleCoordComponent(ctx, valueArg(2, "0.0"), 0);
        auto cy = sampleCoordComponent(ctx, valueArg(3, "0.0"), 1);
        auto ox = ensureScalarIndex(numericArg(6, "0"));
        auto oy = ensureScalarIndex(numericArg(7, "0"));
        auto coord = "float2(" + cx + ", " + cy + ")";
        if (intrinsic_id == DXOP_TextureSampleLevel) {
            auto lod = numericArg(9, "0.0");
            return handle + ".sample(" + samp + ", " + coord +
                   ", level((float)(" + lod + ")), int2(" + ox + ", " + oy + "))";
        }
        if (intrinsic_id == DXOP_TextureSampleBias) {
            auto bias = numericArg(9, "0.0");
            return handle + ".sample(" + samp + ", " + coord +
                   ", bias((float)(" + bias + ")), int2(" + ox + ", " + oy + "))";
        }
        if (intrinsic_id == DXOP_TextureSampleGrad) {
            auto ddx = "float2(" + numericArg(9, "0.0") + ", " +
                       numericArg(10, "0.0") + ")";
            auto ddy = "float2(" + numericArg(12, "0.0") + ", " +
                       numericArg(13, "0.0") + ")";
            return handle + ".sample(" + samp + ", " + coord +
                   ", gradient2d(" + ddx + ", " + ddy + "), int2(" +
                   ox + ", " + oy + "))";
        }
        return handle + ".sample(" + samp + ", " + coord + ", int2(" + ox +
               ", " + oy + "))";
    }
    case DXOP_TextureGather: case DXOP_TextureGatherCmp: case 223: {
        if (args.size() < 4) return "float4(0)";
        auto handle = handleArg(0, "tex", "tex0");
        auto samp = handleArg(1, "samp", "samp0");
        auto cx = sampleCoordComponent(ctx, valueArg(2, "0.0"), 0);
        auto cy = sampleCoordComponent(ctx, valueArg(3, "0.0"), 1);
        auto ox = ensureScalarIndex(numericArg(6, "0"));
        auto oy = ensureScalarIndex(numericArg(7, "0"));
        if (intrinsic_id == DXOP_TextureGatherCmp) {
            auto compare = numericArg(9, "0.0");
            if (ctx.shader.kind == DxilShaderKind::Compute) {
                auto sample = "((" + handle + ".read(uint2((uint)(" + cx +
                               "), (uint)(" + cy + "))) >= (float)(" +
                               compare + ")) ? 1.0f : 0.0f)";
                return sample;
            }
            return handle + ".gather_compare(" + samp + ", float2(" + cx +
                   ", " + cy + "), (float)(" + compare + "), int2(" + ox +
                   ", " + oy + "))";
        }
        if (intrinsic_id == DXOP_TextureGatherRaw || intrinsic_id == 223) {
            return handle + ".gather(" + samp + ", float2(" + cx + ", " +
                   cy + "), int2(" + ox + ", " + oy + "), component::x)";
        }
        uint32_t ch = args.size() > 8 ? literalArg(8, 0, "ch") : 0;
        if (ctx.shader.kind == DxilShaderKind::Compute) {
            auto texel = handle + ".read(uint2(" +
                         textureCoordComponent(ctx, valueArg(2, "0"), 0) + ", " +
                         textureCoordComponent(ctx, valueArg(3, "0"), 1) + "))";
            return "float4((" + texel + ")." + componentName(ch) + ")";
        }
        auto sample = handle + ".sample(" + samp + ", float2(" + cx + ", " +
                       cy + "))";
        return "float4((" + sample + ")." + componentName(ch) + ")";
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
    case DXOP_TextureSampleCmp: case DXOP_TextureSampleCmpLevelZero: case 224: {
        if (args.size() < 10) return "0.0";
        auto handle = handleArg(0, "tex", "tex0");
        auto samp = handleArg(1, "samp", "samp0");
        auto cx = sampleCoordComponent(ctx, valueArg(2, "0.0"), 0);
        auto cy = sampleCoordComponent(ctx, valueArg(3, "0.0"), 1);
        auto cmp = valueArg(9, "0.0");
        auto ox = ensureScalarIndex(numericArg(6, "0"));
        auto oy = ensureScalarIndex(numericArg(7, "0"));
        if (ctx.shader.kind == DxilShaderKind::Compute) {
            std::string sample = handle + ".read(uint2((uint)(" + cx +
                                  "), (uint)(" + cy + "))";
            if (intrinsic_id == DXOP_TextureSampleCmpLevel || intrinsic_id == 224)
                sample += ", (uint)(" + numericArg(10, "0") + ")";
            sample += ")";
            return "(((float)(" + cmp + ") <= " + sample + ") ? 1.0f : 0.0f)";
        }
        if (intrinsic_id == DXOP_TextureSampleCmpLevel || intrinsic_id == 224) {
            auto lod = numericArg(10, "0.0");
            return handle + ".sample_compare(" + samp + ", float2(" + cx +
                   ", " + cy + "), (float)(" + cmp + "), level((float)(" +
                   lod + ")), int2(" + ox + ", " + oy + "))";
        }
        if (intrinsic_id == DXOP_TextureSampleCmpLevelZero)
            return handle + ".sample_compare(" + samp + ", float2(" + cx +
                   ", " + cy + "), (float)(" + cmp + "), level(0.0f), int2(" +
                   ox + ", " + oy + "))";
        return handle + ".sample_compare(" + samp + ", float2(" + cx + ", " +
               cy + "), (float)(" + cmp + "), int2(" + ox + ", " + oy + "))";
    }
    case 70: return "0";
    case 71: return "true";
    case 72: {
        auto handle = handleArg(0, "tex", "tex0");
        return "uint4(" + handle + ".get_width(), " + handle + ".get_height(), 1, 1)";
    }
    case 83: case 85: return "dfdx(" + valueArg(0, "0.0") + ")";
    case 84: case 86: return "dfdy(" + valueArg(0, "0.0") + ")";
    case 81: {
        if (args.size() < 4) return "0.0";
        auto handle = handleArg(0, "tex", "tex0");
        auto samp = handleArg(1, "samp", "samp0");
        return handle + ".calculate_unclamped_lod(" + samp + ", float2(" +
               sampleCoordComponent(ctx, valueArg(2, "0.0"), 0) + ", " +
               sampleCoordComponent(ctx, valueArg(3, "0.0"), 1) + "))";
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
            auto translated =
                "(long)m12_atomic64_binop(reinterpret_cast<volatile device ulong*>(" +
                handle + " + " + byte_offset + "), (ulong)(" + val +
                "), " + std::to_string(op) + "u, m12_atomic64_lock)";
            recordDiagnostic(
                ctx,
                "atomic64 binop handle=%s kind=%u stride=%u op=%u byte_offset=%s value=%s translated=%s",
                handle.c_str(), resource_kind, element_stride, op,
                byte_offset.c_str(), val.c_str(), translated.c_str());
            return translated;
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
    case 75: case 76: case 97: case 98: return "0.5";
    case 77: return "1";
    case 109: return "0";
    case 80: return "threadgroup_barrier(mem_flags::mem_threadgroup)";
    case DXOP_Unary: {
        if (args.size() < 2) return "0";
        uint32_t op = literalArg(0, 0xFFFFFFFFu, "unary");
        bool int_op = op == DXILOP_Bfrev || op == DXILOP_Countbits ||
                      op == DXILOP_FirstbitLo || op == DXILOP_FirstbitHi ||
                      op == DXILOP_FirstbitSHi;
        auto x = numericArg(1, int_op ? "0" : "0.0");
        auto fx = "static_cast<float>(" + x + ")";
        switch (op) {
        case DXILOP_FAbs: return "abs(" + fx + ")";
        case DXILOP_Saturate: return "clamp(" + fx + ", 0.0, 1.0)";
        case DXILOP_IsNaN: return "isnan(" + fx + ")";
        case DXILOP_IsInf: return "isinf(" + fx + ")";
        case DXILOP_IsFinite: return "isfinite(" + fx + ")";
        case DXILOP_Cos: return "cos(" + fx + ")";
        case DXILOP_Sin: return "sin(" + fx + ")";
        case DXILOP_Tan: return "tan(" + fx + ")";
        case DXILOP_Acos: return "acos(" + fx + ")";
        case DXILOP_Asin: return "asin(" + fx + ")";
        case DXILOP_Atan: return "atan(" + fx + ")";
        case DXILOP_Exp: return "exp2(" + fx + ")";
        case DXILOP_Frc: return "fract(" + fx + ")";
        case DXILOP_Log: return "log2(" + fx + ")";
        case DXILOP_Sqrt: return "sqrt(" + fx + ")";
        case DXILOP_Rsqrt: return "rsqrt(" + fx + ")";
        case DXILOP_Round_ne: return "rint(" + fx + ")";
        case DXILOP_Round_ni: return "floor(" + fx + ")";
        case DXILOP_Round_pi: return "ceil(" + fx + ")";
        case DXILOP_Round_z: return "trunc(" + fx + ")";
        case DXILOP_Bfrev: return "reverse_bits(" + x + ")";
        case DXILOP_Countbits: return "popcount(static_cast<uint>(" + x + "))";
        case DXILOP_FirstbitLo: return "ctz(" + x + ")";
        case DXILOP_FirstbitHi: return "clz(" + x + ")";
        case DXILOP_FirstbitSHi: return "((" + x + ") < 0 ? clz(~(" + x + ")) : clz(" + x + "))";
        default: ctx.unsupported_intrinsics++; return x;
        }
    }
    case DXOP_Binary: {
        if (args.size() < 3) return "0";
        uint32_t op = literalArg(0, 0xFFFFFFFFu, "binary");
        auto a = numericArg(1, "0"), b = numericArg(2, "0");
        switch (op) {
        case DXILOP_FMax: return "max(static_cast<float>(" + a + "), static_cast<float>(" + b + "))";
        case DXILOP_FMin: return "min(static_cast<float>(" + a + "), static_cast<float>(" + b + "))";
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
        auto a = numericArg(1, "0"), b = numericArg(2, "0"), c = numericArg(3, "0");
        switch (op) {
        case DXILOP_FMad: case DXILOP_Fma:
            return "fma(static_cast<float>(" + a + "), static_cast<float>(" + b +
                   "), static_cast<float>(" + c + "))";
        case DXILOP_IMad: case DXILOP_UMad: return "((" + a + ") * (" + b + ") + (" + c + "))";
        case DXILOP_Ibfe: return "extract_bits(" + a + ", " + b + ", " + c + ")";
        case DXILOP_Ubfe: return "extract_bits((uint)(" + a + "), (uint)(" + b + "), (uint)(" + c + "))";
        case DXILOP_Bfi: return "insert_bits((uint)(" + b + "), (uint)(" + a + "), (uint)(" + c + "))";
        default: ctx.unsupported_intrinsics++; return a;
        }
    }
    case DXOP_Dot2: {
        if (args.size() < 4) return "0.0";
        return "((" + numericArg(0,"0.0") + ")*(" + numericArg(2,"0.0") + ") + (" + numericArg(1,"0.0") + ")*(" + numericArg(3,"0.0") + "))";
    }
    case DXOP_Dot3: {
        if (args.size() < 6) return "0.0";
        return "((" + numericArg(0,"0.0") + ")*(" + numericArg(3,"0.0") + ") + (" + numericArg(1,"0.0") + ")*(" + numericArg(4,"0.0") + ") + (" + numericArg(2,"0.0") + ")*(" + numericArg(5,"0.0") + "))";
    }
    case DXOP_Dot4: {
        if (args.size() < 8) return "0.0";
        return "((" + numericArg(0,"0.0") + ")*(" + numericArg(4,"0.0") + ") + (" + numericArg(1,"0.0") + ")*(" + numericArg(5,"0.0") + ") + (" + numericArg(2,"0.0") + ")*(" + numericArg(6,"0.0") + ") + (" + numericArg(3,"0.0") + ")*(" + numericArg(7,"0.0") + "))";
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
            return varyingField("in", input_id) + componentSuffix(comp);
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
    case 131: return "static_cast<float>(half(" + numericArg(1, "0") + "))";
    case 132: return "static_cast<uint>(half(" + numericArg(1, "0.0") + "))";
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
    case DXOP_WaveActiveOp: {
        auto value = numericArg(0, "0");
        uint32_t op = literalArg(1, 0xFFFFFFFFu, "wave_active_op");
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
        switch (op) {
        case 0: return "simd_prefix_exclusive_sum(" + value + ")";
        case 1: return "simd_prefix_exclusive_product(" + value + ")";
        default: ctx.unsupported_intrinsics++; return value;
        }
    }
    case DXOP_QuadReadLaneAt:
        return "quad_broadcast(" + numericArg(0, "0") + ", (uint)(" + numericArg(1, "0") + "))";
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
        if (startsWith(expr, "buf"))
            return {MSLTypeKind::DeviceCharPtr, 0, {}};
        if (startsWith(expr, "tex"))
            return {MSLTypeKind::RWTexture2D, 0, {}};
        if (startsWith(expr, "samp"))
            return {MSLTypeKind::Sampler, 0, {}};
        if (exprLooksScalarMathCall(expr))
            return {MSLTypeKind::Float, 0, {}};
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
        if (expr.find(".gather(") != std::string::npos)
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
             DXILIRBuilder::isVectorType(resolved)))
            return resolved;
        MSLType inferred = inferTypeFromExpr(getValue(idx));
        if (isUsableType(inferred) &&
            (!isUsableType(tracked) || DXILIRBuilder::isVectorType(inferred)))
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
            return {MSLTypeKind::Int, 0, {}};
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

        if (DXILIRBuilder::isVectorType(op0)) result_type = op0;
        else if (DXILIRBuilder::isVectorType(op1)) result_type = op1;
        else if (DXILIRBuilder::isFloatType(op0) || DXILIRBuilder::isFloatType(op1))
            result_type = {pointer_scalar == MSLTypeKind::Float ? MSLTypeKind::Float : MSLTypeKind::Float, 0, {}};
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
                intrinsic_id = opcode;
                opcode_prefixed_intrinsic = true;
            }
        }

        if (intrinsic_id != 0 && call_args.empty()) {
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
            if (opcode_prefixed_intrinsic)
                fn_args.assign(call_args.begin() + 1, call_args.end());
            else if (intrinsic_id == 13 || intrinsic_id == 14 || intrinsic_id == 15)
                fn_args = call_args;
            else
                fn_args.assign(call_args.begin() + 1, call_args.end());

            std::string translated = translateDXIntrinsic(ctx, intrinsic_id, fn_args, callee_name);
            MSLType result_type = inferDXIntrinsicResultType(
                ctx, intrinsic_id, fn_args, bestType(getTypeForInst(inst.type_id), translated),
                callee_name);
            if (translated.find(".sample_compare(") != std::string::npos)
                result_type = {MSLTypeKind::Float, 0, {}};
            if (intrinsic_id == DXOP_AtomicBinOp)
                recordDiagnostic(ctx,
                                 "atomic binop emit value=%u callee=%s type=%u translated=%s",
                                 value_counter, callee_name.c_str(),
                                 static_cast<unsigned>(result_type.kind),
                                 translated.c_str());
            if (intrinsic_id == DXOP_CBufferLoad || intrinsic_id == DXOP_CBufferLoadLegacy) {
                const char *handle_value = "<missing>";
                uint32_t handle_id = fn_args.empty() ? UINT32_MAX : fn_args[0];
                if (handle_id < ctx.value_table.size() && !ctx.value_table[handle_id].empty())
                    handle_value = ctx.value_table[handle_id].c_str();
                recordDiagnostic(ctx,
                                 "cbufferLoad value=%u handle_id=%u handle=%s translated=%s",
                                 value_counter, handle_id, handle_value, translated.c_str());
            }
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
            } else if (translated.find('=') == std::string::npos) {
                if (!translated.empty() && translated[0] != ' ') {
                    bool is_resource_handle = startsWith(translated, "buf") ||
                                              startsWith(translated, "tex") ||
                                              startsWith(translated, "samp");
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
        } else {
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
        auto pre_it = ctx.predeclared_types.find(result);
        if (pre_it != ctx.predeclared_types.end() &&
            DXILIRBuilder::isVectorType(pre_it->second) &&
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
            !DXILIRBuilder::isVectorType(result_type))
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
        std::string expr = preserve_float_arithmetic &&
                           (inst.opcode == LLVMInstruction::URem ||
                            inst.opcode == LLVMInstruction::SRem)
            ? "fmod(" + lhs + ", " + rhs + ")"
            : lhs + " " + std::string(op_str) + " " + rhs;
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
            bool agg_is_vector = DXILIRBuilder::isVectorType(agg_type);

            if (is_struct) {
                if (idx == 0) {
                    emitTypedLine(result_type, result, agg);
                } else {
                    emitTypedLine(result_type, result, defaultForType(result_type));
                }
            } else if (agg_is_vector && idx < 4) {
                std::string expr = componentAccess(agg, idx, agg_type);
                auto scalar = DXILIRBuilder::scalarType(agg_type);
                emitTypedLine(scalar, result, expr);
                result_type = scalar;
            } else if (agg_is_vector == false && agg_type.kind != MSLTypeKind::Unknown) {
                emitTypedLine(result_type, result, agg);
            } else {
                auto inferred = inferTypeFromExpr(agg);
                if (DXILIRBuilder::isVectorType(inferred) && idx < 4) {
                    std::string expr = componentAccess(agg, idx, inferred);
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
        if (inst.operands.size() >= 3) {
            auto vec = getValue(inst.operands[0]);
            auto elem = getValue(inst.operands[1]);
            auto idx = getValue(inst.operands[2]);
            emitTypedLine(result_type, result, vec);
            uint32_t idx_val = 0;
            if (parseUnsignedLiteral(idx, idx_val) && idx_val < 4)
                os << "  " << result << componentSuffix(idx_val) << " = " << elem << ";\n";
            else
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
            emitTypedLine(result_type, result, "-(" + getValue(inst.operands[0]) + ")");
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
            result_type = chooseBinaryType(result_type, operandType(inst.operands[0]),
                                           operandType(inst.operands[1]), MSLTypeKind::Float);
            std::string lhs = coerceOperand(inst.operands[0], result_type);
            std::string rhs = coerceOperand(inst.operands[1], result_type);
            if (inst.opcode == LLVMInstruction::FRem &&
                !DXILIRBuilder::isVectorType(result_type)) {
                lhs = castExpr(lhs, {MSLTypeKind::Float, 0, {}});
                rhs = castExpr(rhs, {MSLTypeKind::Float, 0, {}});
            }
            std::string expr = inst.opcode == LLVMInstruction::FRem
                ? "fmod(" + lhs + ", " + rhs + ")"
                : lhs + std::string(" ") + fop + " " + rhs;
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
            if (isPointerType(source_type) || typeLooksResourceHandle(source_type) ||
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
            const char *cmp = "==";
            MSLType result_type = {MSLTypeKind::Bool, 0, {}};
            std::string cmp_result;
            if (inst.opcode == LLVMInstruction::ICmp) {
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
            } else if (pred == 0) {
                cmp_result = "false";
            } else if (pred == 15) {
                cmp_result = "true";
            } else if (pred == 7) {
                std::string ilhs = coerceIsNanOperand(lhs, cmp_type);
                std::string irhs = coerceIsNanOperand(rhs, cmp_type);
                if (DXILIRBuilder::isVectorType(cmp_type))
                    cmp_result = "all((!isnan(" + ilhs + ")) & (!isnan(" + irhs + ")))";
                else
                    cmp_result = "(!isnan(" + ilhs + ") && !isnan(" + irhs + "))";
            } else if (pred == 14) {
                std::string ilhs = coerceIsNanOperand(lhs, cmp_type);
                std::string irhs = coerceIsNanOperand(rhs, cmp_type);
                if (DXILIRBuilder::isVectorType(cmp_type))
                    cmp_result = "any((isnan(" + ilhs + ")) | (isnan(" + irhs + ")))";
                else
                    cmp_result = "(isnan(" + ilhs + ") || isnan(" + irhs + "))";
            } else {
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
            ctx.value_types[value_counter] = {MSLTypeKind::Bool, 0, {}};
        }
        value_counter++;
        break;
    }

    case LLVMInstruction::Select: {
        ensureValueTable(value_counter);
        MSLType result_type = getTypeForInst(inst.type_id);
        if (inst.operands.size() >= 3) {
            auto cond = coerceOperand(inst.operands[0], {MSLTypeKind::Bool, 0, {}});
            MSLType tv_type = operandType(inst.operands[1]);
            MSLType fv_type = operandType(inst.operands[2]);
            result_type = chooseBinaryType(result_type, tv_type, fv_type, MSLTypeKind::Int);
            if (DXILIRBuilder::isVectorType(tv_type))
                result_type = tv_type;
            else if (DXILIRBuilder::isVectorType(fv_type))
                result_type = fv_type;
            auto pre_it = ctx.predeclared_types.find(result);
            if (pre_it != ctx.predeclared_types.end() &&
                (DXILIRBuilder::isFloatType(pre_it->second) || DXILIRBuilder::isIntType(pre_it->second)) &&
                !DXILIRBuilder::isVectorType(pre_it->second))
                result_type = pre_it->second;
            auto tv = coerceOperand(inst.operands[1], result_type);
            auto fv = coerceOperand(inst.operands[2], result_type);
            emitTypedLine(result_type, result, "(" + cond + " ? " + tv + " : " + fv + ")");
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
            recordDiagnostic(ctx,
                             "group atomic64 value=%u llvm_op=%u m12_op=%u ptr=%s value=%s",
                             value_counter, llvm_op, m12_op, ptr.c_str(),
                             val.c_str());
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
            std::string type_name = emitTypeName(result_type);
            std::string expr = defaultForType(result_type);
            if (startsWith(ptr, "tex") || startsWith(ptr, "samp") ||
                exprContainsRawResourceHandle(ptr) || exprLooksScalarLiteral(ptr) ||
                exprLooksVectorValue(ptr) || DXILIRBuilder::isVectorType(ptr_name_type)) {
                expr = defaultForType(result_type);
            } else if (isPointerType(ptr_type) ||
                       ptr.find("threadgroup") != std::string::npos ||
                       ptr.find("device") != std::string::npos ||
                       ptr.find("gvar_") != std::string::npos) {
                expr = "*reinterpret_cast<" + std::string(pointerAddressSpace(inst.operands[0])) + " " +
                       type_name + "*>(" + ptr + ")";
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
            if (idx != "0" && idx != "0.0" && idx != "0.0f")
                gep += " + " + idx;
        }
        ctx.value_table[value_counter] = gep;
        ctx.value_types[value_counter] = {MSLTypeKind::DeviceCharPtr, 0, {}};
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
    ctx.compute_sample_cmp_shader = shader.kind == DxilShaderKind::Compute;
    if (ctx.compute_sample_cmp_shader) {
        ctx.compute_sample_cmp_shader = false;
        for (const auto &decl : module.functions) {
            if (startsWith(decl.name, "dx.op.sampleCmp") ||
                startsWith(decl.name, "dx.op.textureGatherCmp")) {
                ctx.compute_sample_cmp_shader = true;
                break;
            }
        }
    }
    ctx.compute_texture_sample_shader = shader.kind == DxilShaderKind::Compute;
    if (ctx.compute_texture_sample_shader) {
        ctx.compute_texture_sample_shader = false;
        for (const auto &decl : module.functions) {
            if (startsWith(decl.name, "dx.op.sample") ||
                startsWith(decl.name, "dx.op.textureGather")) {
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
                    std::vector<uint32_t> fn_args;
                    if (intrinsic_id == DXOP_Unary || intrinsic_id == DXOP_Binary ||
                        intrinsic_id == DXOP_Tertiary)
                        fn_args.assign(inst.operands.begin() + 2, inst.operands.end());
                    else
                        fn_args.assign(inst.operands.begin() + 3, inst.operands.end());
                    MSLType declared = DXILIRBuilder::resolveType(inst.type_id, module);
                    MSLType inferred = inferDXIntrinsicResultType(ctx, intrinsic_id, fn_args, declared,
                                                                  callee_name);
                    if (inferred.kind != MSLTypeKind::Unknown && inferred.kind != MSLTypeKind::Void)
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
            if (DXILIRBuilder::isVectorType(source_type))
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
                DXILIRBuilder::isVectorType(ctx.value_types[inst.operands[0]]))
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
