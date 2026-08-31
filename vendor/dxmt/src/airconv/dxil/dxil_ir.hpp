#pragma once

#include "llvm_bitcode.hpp"
#include "dxil_container.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstdint>

namespace dxmt::dxil {

enum class MSLTypeKind : uint8_t {
    Void,
    Bool,
    Float,
    Float2,
    Float3,
    Float4,
    Int,
    Int2,
    Int3,
    Int4,
    UInt,
    UInt2,
    UInt3,
    UInt4,
    Half,
    Short,
    UShort,
    Long,
    Double,
    LongVector,
    DeviceCharPtr,
    ThreadgroupCharPtr,
    Texture2D,
    Texture2DArray,
    Texture3D,
    TextureCube,
    Texture2DMS,
    RWTexture2D,
    RWTexture2DArray,
    RWTexture3D,
    Sampler,
    Struct,
    Unknown,
};

struct MSLType {
    MSLTypeKind kind = MSLTypeKind::Unknown;
    uint32_t struct_type_id = 0;
    std::vector<MSLType> struct_fields;
    // SM6.9 permits vectors wider than Metal's native float2/3/4 types. Such
    // values are represented as array<scalar, vector_width> and scalarized
    // by operations that consume them.
    uint32_t vector_width = 0;
    MSLTypeKind vector_element_kind = MSLTypeKind::Unknown;
};

enum class ValueRole : uint8_t {
    Generic,
    BufferHandle,
    TextureHandle,
    SamplerHandle,
    ThreadID,
    GroupID,
    GroupThreadID,
    Constant,
    Function,
};

struct TypedValue {
    uint32_t ssa_id = 0;
    MSLType type;
    ValueRole role = ValueRole::Generic;
    uint32_t binding_index = UINT32_MAX;
    std::string resolved_name;
    std::string expr;
};

struct TypedInstruction {
    enum Kind {
        Nop,
        BinaryOp,
        UnaryOp,
        CompareOp,
        CastOp,
        ExtractValue,
        InsertValue,
        ExtractElement,
        InsertElement,
        ShuffleVector,
        Load,
        Store,
        GEP,
        Alloca,
        Call,
        Phi,
        Branch,
        Switch,
        Return,
        Unreachable,
    } kind = Nop;

    uint32_t result_id = UINT32_MAX;
    MSLType result_type;
    ValueRole result_role = ValueRole::Generic;
    uint32_t result_binding = UINT32_MAX;

    LLVMInstruction::Opcode llvm_opcode = (LLVMInstruction::Opcode)0;

    std::vector<uint32_t> operands;

    uint32_t intrinsic_id = 0;
    uint32_t cmp_predicate = 0;

    std::string raw_expr;
};

struct TypedBlock {
    std::string name;
    std::vector<TypedInstruction> instructions;
};

struct TypedFunction {
    std::string name;
    uint32_t value_id = 0;
    MSLType return_type;
    std::vector<TypedValue> params;
    std::vector<TypedBlock> blocks;
    std::vector<uint32_t> block_value_ids;
    uint32_t instruction_start_value = 0;
};

struct BindingInfo {
    enum class Kind { Buffer, Texture, RWTexture, Sampler } kind;
    uint32_t register_space = 0;
    uint32_t register_index = 0;
    uint32_t count = 1;
    std::string name;
    MSLType element_type;
};

struct TypedModule {
    TypedFunction entry = {};
    std::vector<BindingInfo> bindings;
    std::vector<TypedValue> constants;
    std::vector<TypedFunction> function_decls;
    uint32_t num_threads[3] = {1, 1, 1};
    DxilShaderKind shader_kind = DxilShaderKind::Invalid;
    std::string entry_point;
};

class DXILIRBuilder {
public:
    static std::optional<TypedModule> build(const LLVMModule &module,
                                             const DxilParsedShader &shader);

    static MSLType resolveType(uint32_t type_id, const LLVMModule &mod);
    static std::string mslTypeName(const MSLType &t);
    static uint32_t typeBitWidth(const MSLType &t);
    static bool isFloatType(const MSLType &t);
    static bool isIntType(const MSLType &t);
    static bool isVectorType(const MSLType &t);
    static bool isLongVectorType(const MSLType &t);
    static uint32_t vectorWidth(const MSLType &t);
    static MSLType scalarType(const MSLType &t);
    static MSLType vectorOfType(const MSLType &elem, uint32_t width);
};

}
