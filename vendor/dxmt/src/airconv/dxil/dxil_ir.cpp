#include "dxil_ir.hpp"
#include "dxil_container.hpp"
#include <sstream>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>

namespace dxmt::dxil {

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

static std::vector<std::string> parseAggregateLiteral(const std::string &text) {
    std::vector<std::string> values;
    bool is_agg = startsWith(text, "agg(") && text.size() >= 5 && text.back() == ')';
    bool is_brace = !text.empty() && text[0] == '{' && text.back() == '}';
    if (!is_agg && !is_brace) return values;
    size_t start = is_agg ? 4 : 1;
    while (start < text.size() - 1) {
        size_t comma = text.find(',', start);
        size_t end = comma == std::string::npos ? text.size() - 1 : comma;
        std::string val = text.substr(start, end - start);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
            val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
            val.pop_back();
        if (!val.empty()) values.push_back(val);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return values;
}

static std::string ensureScalarIndex(const std::string &val) {
    if (startsWith(val, "agg(")) {
        auto parts = parseAggregateLiteral(val);
        return parts.empty() ? "0" : parts[0];
    }
    return val;
}

MSLType DXILIRBuilder::resolveType(uint32_t type_id, const LLVMModule &mod) {
    if (type_id == 0 || type_id >= mod.types.size())
        return {MSLTypeKind::Unknown, 0, {}};
    auto &t = mod.types[type_id];
    switch (t.kind) {
    case LLVMType::Void: return {MSLTypeKind::Void, 0, {}};
    case LLVMType::Float: return {MSLTypeKind::Float, 0, {}};
    case LLVMType::Double: return {MSLTypeKind::Double, 0, {}};
    case LLVMType::Integer: {
        if (t.bit_width == 1) return {MSLTypeKind::Bool, 0, {}};
        if (t.bit_width == 16) return {MSLTypeKind::Short, 0, {}};
        if (t.bit_width == 32) return {MSLTypeKind::Int, 0, {}};
        if (t.bit_width == 64) return {MSLTypeKind::Long, 0, {}};
        return {MSLTypeKind::Int, 0, {}};
    }
    case LLVMType::Pointer: {
        if (t.address_space == 3)
            return {MSLTypeKind::ThreadgroupCharPtr, 0, {}};
        return {MSLTypeKind::DeviceCharPtr, 0, {}};
    }
    case LLVMType::Vector: {
        if (t.subtypes.empty())
            return {MSLTypeKind::Unknown, 0, {}};
        auto elem = resolveType(type_id, mod);
        auto &elem_type = t.subtypes[0];
        uint32_t count = t.bit_width;
        if (count == 2) {
            if (elem_type.kind == LLVMType::Float) return {MSLTypeKind::Float2, 0, {}};
            if (elem_type.kind == LLVMType::Integer && elem_type.bit_width == 32) return {MSLTypeKind::Int2, 0, {}};
            if (elem_type.kind == LLVMType::Integer && elem_type.bit_width == 16) return {MSLTypeKind::Short, 0, {}};
            return {MSLTypeKind::Float2, 0, {}};
        }
        if (count == 3) {
            if (elem_type.kind == LLVMType::Float) return {MSLTypeKind::Float3, 0, {}};
            if (elem_type.kind == LLVMType::Integer && elem_type.bit_width == 32) return {MSLTypeKind::Int3, 0, {}};
            return {MSLTypeKind::Float3, 0, {}};
        }
        if (count == 4) {
            if (elem_type.kind == LLVMType::Float) return {MSLTypeKind::Float4, 0, {}};
            if (elem_type.kind == LLVMType::Integer && elem_type.bit_width == 32) return {MSLTypeKind::Int4, 0, {}};
            if (elem_type.kind == LLVMType::Integer && elem_type.bit_width == 16) return {MSLTypeKind::Short, 0, {}};
            return {MSLTypeKind::Float4, 0, {}};
        }
        return {MSLTypeKind::Unknown, 0, {}};
    }
    case LLVMType::Struct: {
        MSLType result;
        result.kind = MSLTypeKind::Struct;
        result.struct_type_id = type_id;
        for (auto &ref : t.type_refs) {
            result.struct_fields.push_back(resolveType(ref, mod));
        }
        return result;
    }
    case LLVMType::Array:
        return resolveType(t.subtypes.empty() ? 0 : type_id, mod);
    case LLVMType::Function:
        return {MSLTypeKind::Void, 0, {}};
    }
    return {MSLTypeKind::Unknown, 0, {}};
}

std::string DXILIRBuilder::mslTypeName(const MSLType &t) {
    switch (t.kind) {
    case MSLTypeKind::Void: return "void";
    case MSLTypeKind::Bool: return "bool";
    case MSLTypeKind::Float: return "float";
    case MSLTypeKind::Float2: return "float2";
    case MSLTypeKind::Float3: return "float3";
    case MSLTypeKind::Float4: return "float4";
    case MSLTypeKind::Int: return "int";
    case MSLTypeKind::Int2: return "int2";
    case MSLTypeKind::Int3: return "int3";
    case MSLTypeKind::Int4: return "int4";
    case MSLTypeKind::UInt: return "uint";
    case MSLTypeKind::UInt2: return "uint2";
    case MSLTypeKind::UInt3: return "uint3";
    case MSLTypeKind::UInt4: return "uint4";
    case MSLTypeKind::Half: return "half";
    case MSLTypeKind::Short: return "short";
    case MSLTypeKind::UShort: return "ushort";
    case MSLTypeKind::Long: return "long";
    case MSLTypeKind::Double: return "float64_t";
    case MSLTypeKind::DeviceCharPtr: return "device char*";
    case MSLTypeKind::ThreadgroupCharPtr: return "threadgroup char*";
    case MSLTypeKind::Texture2D: return "texture2d<float, access::sample>";
    case MSLTypeKind::Texture2DArray: return "texture2d_array<float, access::sample>";
    case MSLTypeKind::Texture3D: return "texture3d<float, access::sample>";
    case MSLTypeKind::TextureCube: return "texturecube<float, access::sample>";
    case MSLTypeKind::Texture2DMS: return "texture2d_ms<float, access::read>";
    case MSLTypeKind::RWTexture2D: return "texture2d<float, access::read_write>";
    case MSLTypeKind::RWTexture2DArray: return "texture2d_array<float, access::read_write>";
    case MSLTypeKind::RWTexture3D: return "texture3d<float, access::read_write>";
    case MSLTypeKind::Sampler: return "sampler";
    case MSLTypeKind::Struct: return "struct_" + std::to_string(t.struct_type_id);
    default: return "int";
    }
}

uint32_t DXILIRBuilder::typeBitWidth(const MSLType &t) {
    switch (t.kind) {
    case MSLTypeKind::Bool: return 1;
    case MSLTypeKind::Float: case MSLTypeKind::Int: case MSLTypeKind::UInt: return 32;
    case MSLTypeKind::Float2: case MSLTypeKind::Int2: case MSLTypeKind::UInt2: return 64;
    case MSLTypeKind::Float3: case MSLTypeKind::Int3: case MSLTypeKind::UInt3: return 96;
    case MSLTypeKind::Float4: case MSLTypeKind::Int4: case MSLTypeKind::UInt4: return 128;
    case MSLTypeKind::Double: case MSLTypeKind::Long: return 64;
    case MSLTypeKind::Half: case MSLTypeKind::Short: case MSLTypeKind::UShort: return 16;
    default: return 32;
    }
}

bool DXILIRBuilder::isFloatType(const MSLType &t) {
    return t.kind == MSLTypeKind::Float || t.kind == MSLTypeKind::Float2 ||
           t.kind == MSLTypeKind::Float3 || t.kind == MSLTypeKind::Float4 ||
           t.kind == MSLTypeKind::Double || t.kind == MSLTypeKind::Half;
}

bool DXILIRBuilder::isIntType(const MSLType &t) {
    return t.kind == MSLTypeKind::Int || t.kind == MSLTypeKind::Int2 ||
           t.kind == MSLTypeKind::Int3 || t.kind == MSLTypeKind::Int4 ||
           t.kind == MSLTypeKind::UInt || t.kind == MSLTypeKind::UInt2 ||
           t.kind == MSLTypeKind::UInt3 || t.kind == MSLTypeKind::UInt4 ||
           t.kind == MSLTypeKind::Bool || t.kind == MSLTypeKind::Short ||
           t.kind == MSLTypeKind::UShort || t.kind == MSLTypeKind::Long;
}

bool DXILIRBuilder::isVectorType(const MSLType &t) {
    return t.kind == MSLTypeKind::Float2 || t.kind == MSLTypeKind::Float3 || t.kind == MSLTypeKind::Float4 ||
           t.kind == MSLTypeKind::Int2 || t.kind == MSLTypeKind::Int3 || t.kind == MSLTypeKind::Int4 ||
           t.kind == MSLTypeKind::UInt2 || t.kind == MSLTypeKind::UInt3 || t.kind == MSLTypeKind::UInt4;
}

uint32_t DXILIRBuilder::vectorWidth(const MSLType &t) {
    switch (t.kind) {
    case MSLTypeKind::Float2: case MSLTypeKind::Int2: case MSLTypeKind::UInt2: return 2;
    case MSLTypeKind::Float3: case MSLTypeKind::Int3: case MSLTypeKind::UInt3: return 3;
    case MSLTypeKind::Float4: case MSLTypeKind::Int4: case MSLTypeKind::UInt4: return 4;
    default: return 1;
    }
}

MSLType DXILIRBuilder::scalarType(const MSLType &t) {
    switch (t.kind) {
    case MSLTypeKind::Float2: case MSLTypeKind::Float3: case MSLTypeKind::Float4:
        return {MSLTypeKind::Float, 0, {}};
    case MSLTypeKind::Int2: case MSLTypeKind::Int3: case MSLTypeKind::Int4:
        return {MSLTypeKind::Int, 0, {}};
    case MSLTypeKind::UInt2: case MSLTypeKind::UInt3: case MSLTypeKind::UInt4:
        return {MSLTypeKind::UInt, 0, {}};
    default: return t;
    }
}

MSLType DXILIRBuilder::vectorOfType(const MSLType &elem, uint32_t width) {
    bool is_f = (elem.kind == MSLTypeKind::Float || elem.kind == MSLTypeKind::Half);
    bool is_i = (elem.kind == MSLTypeKind::Int || elem.kind == MSLTypeKind::UInt);
    if (is_f) {
        if (width == 2) return {MSLTypeKind::Float2, 0, {}};
        if (width == 3) return {MSLTypeKind::Float3, 0, {}};
        if (width == 4) return {MSLTypeKind::Float4, 0, {}};
    }
    if (is_i) {
        if (elem.kind == MSLTypeKind::UInt) {
            if (width == 2) return {MSLTypeKind::UInt2, 0, {}};
            if (width == 3) return {MSLTypeKind::UInt3, 0, {}};
            if (width == 4) return {MSLTypeKind::UInt4, 0, {}};
        }
        if (width == 2) return {MSLTypeKind::Int2, 0, {}};
        if (width == 3) return {MSLTypeKind::Int3, 0, {}};
        if (width == 4) return {MSLTypeKind::Int4, 0, {}};
    }
    return elem;
}

static uint32_t resolveFieldTypeId(const LLVMModule &mod, uint32_t struct_type_id, uint32_t field_idx) {
    if (struct_type_id == 0 || struct_type_id >= mod.types.size()) return 0;
    auto &st = mod.types[struct_type_id];
    if (st.kind != LLVMType::Struct) return 0;
    if (field_idx >= st.type_refs.size()) return 0;
    return st.type_refs[field_idx];
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

static uint32_t intrinsicIdFromCalleeName(const std::string &name) {
    if (name.size() < 6 || name.substr(0, 5) != "dx.op")
        return 0;
    auto dot = name.rfind('.');
    if (dot == std::string::npos) {
        char *end = nullptr;
        auto id_str = name.substr(5);
        while (!id_str.empty() && (id_str[0] < '0' || id_str[0] > '9'))
            id_str.erase(id_str.begin());
        uint32_t id = std::strtoul(id_str.c_str(), &end, 10);
        return (end && *end == '\0') ? id : 0;
    }
    char *end = nullptr;
    uint32_t id = std::strtoul(name.c_str() + dot + 1, &end, 10);
    return (end && *end == '\0') ? id : 0;
}

static std::string emitValue(uint32_t idx) {
    if (idx == 0xFFFFFFFF) return "undef";
    return "v" + std::to_string(idx);
}

std::optional<TypedModule> DXILIRBuilder::build(const LLVMModule &module,
                                                  const DxilParsedShader &shader) {
    if (module.functions.empty())
        return std::nullopt;

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
    if (!entry_fn) return std::nullopt;

    TypedModule tmod;
    tmod.shader_kind = shader.kind;
    tmod.entry_point = shader.entry_point;
    tmod.num_threads[0] = module.num_threads[0];
    tmod.num_threads[1] = module.num_threads[1];
    tmod.num_threads[2] = module.num_threads[2];

    struct ValueInfo {
        MSLType type;
        ValueRole role = ValueRole::Generic;
        uint32_t binding_index = UINT32_MAX;
        std::string resolved_name;
        std::string expr;
        bool has_expr = false;
    };

    std::unordered_map<uint32_t, ValueInfo> values;

    auto setType = [&](uint32_t id, uint32_t type_id) {
        values[id].type = resolveType(type_id, module);
    };

    auto setRole = [&](uint32_t id, ValueRole role) {
        values[id].role = role;
    };

    auto setBinding = [&](uint32_t id, uint32_t binding) {
        values[id].binding_index = binding;
    };

    auto setExpr = [&](uint32_t id, const std::string &expr) {
        values[id].expr = expr;
        values[id].has_expr = true;
    };

    auto setName = [&](uint32_t id, const std::string &name) {
        values[id].resolved_name = name;
    };

    auto getType = [&](uint32_t id) -> MSLType {
        auto it = values.find(id);
        if (it != values.end()) return it->second.type;
        for (auto &c : module.constants) {
            if (c.id == id) return resolveType(c.type_id, module);
        }
        for (auto &c : entry_fn->constants) {
            if (c.id == id) return resolveType(c.type_id, module);
        }
        return {MSLTypeKind::Unknown, 0, {}};
    };

    auto getRole = [&](uint32_t id) -> ValueRole {
        auto it = values.find(id);
        if (it != values.end()) return it->second.role;
        return ValueRole::Generic;
    };

    auto getExpr = [&](uint32_t id) -> std::string {
        auto it = values.find(id);
        if (it != values.end() && it->second.has_expr)
            return it->second.expr;
        return emitValue(id);
    };

    auto parseUnsignedFromExpr = [&](uint32_t id) -> uint32_t {
        auto expr = getExpr(id);
        if (expr.empty()) return 0;
        char *end = nullptr;
        unsigned long v = std::strtoul(expr.c_str(), &end, 10);
        return (end && *end == '\0') ? (uint32_t)v : 0;
    };

    for (auto &c : module.constants) {
        setType(c.id, c.type_id);
        if (!c.constant_data.empty())
            setExpr(c.id, c.constant_data);
    }
    for (auto &c : entry_fn->constants) {
        setType(c.id, c.type_id);
        if (!c.constant_data.empty())
            setExpr(c.id, c.constant_data);
    }

    for (auto &gv : module.globals) {
        setType(gv.value_id, gv.type_id);
        if (!gv.name.empty()) {
            std::string esc;
            for (char c : gv.name) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_')
                    esc += c;
                else
                    esc += '_';
            }
            if (!esc.empty() && esc[0] >= '0' && esc[0] <= '9')
                esc = "_" + esc;
            setName(gv.value_id, esc);
            setExpr(gv.value_id, esc);
        }
    }

    for (auto &fn : module.functions) {
        if (!fn.is_declaration || fn.name.empty()) continue;
        setType(fn.value_id, fn.type_id);
        setExpr(fn.value_id, fn.name);
        setRole(fn.value_id, ValueRole::Function);
    }

    TypedFunction tf;
    tf.name = entry_fn->name;
    tf.value_id = entry_fn->value_id;
    tf.return_type = resolveType(entry_fn->return_type.kind != LLVMType::Void ? entry_fn->type_id : 0, module);
    tf.instruction_start_value = entry_fn->instruction_start_value;
    tf.block_value_ids = entry_fn->block_value_ids;

    for (uint32_t i = 0; i < entry_fn->param_count; i++) {
        TypedValue pv;
        uint32_t param_val_id = tf.instruction_start_value + i;
        pv.ssa_id = param_val_id;
        if (i < entry_fn->param_types.size())
            pv.type = resolveType(entry_fn->param_types[i].kind != LLVMType::Void ? i : 0, module);
        pv.role = ValueRole::Generic;
        pv.resolved_name = emitValue(param_val_id);
        tf.params.push_back(pv);
    }

    std::string last_buffer_handle;

    for (size_t bi = 0; bi < entry_fn->blocks.size(); bi++) {
        auto &block = entry_fn->blocks[bi];
        TypedBlock tb;
        tb.name = block.name.empty() ? "bb" + std::to_string(bi) : block.name;

        uint32_t vc = entry_fn->instruction_start_value;
        if (bi > 0) {
            for (size_t prev = 0; prev < bi; prev++) {
                for (auto &inst : entry_fn->blocks[prev].instructions) {
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

        for (auto &inst : block.instructions) {
            TypedInstruction ti;
            ti.llvm_opcode = inst.opcode;

            bool produces_value = true;
            switch (inst.opcode) {
            case LLVMInstruction::Ret:
            case LLVMInstruction::Br:
            case LLVMInstruction::Switch:
            case LLVMInstruction::Unreachable:
            case LLVMInstruction::Store:
                produces_value = false;
                break;
            default:
                break;
            }

            if (produces_value) {
                ti.result_id = vc;
                if (inst.type_id > 0 && inst.type_id < module.types.size())
                    ti.result_type = resolveType(inst.type_id, module);
            }

            switch (inst.opcode) {
            case LLVMInstruction::Call: {
                if (inst.operands.empty()) {
                    ti.kind = TypedInstruction::Nop;
                    ti.raw_expr = "  // empty call\n";
                    break;
                }

                uint32_t callee = inst.operands[0];
                std::string callee_name = getExpr(callee);
                uint32_t intrinsic_id = intrinsicIdFromCalleeName(callee_name);

                std::vector<uint32_t> call_args;
                for (size_t i = 2; i < inst.operands.size(); i++)
                    call_args.push_back(inst.operands[i]);

                if (intrinsic_id != 0) {
                    ti.kind = TypedInstruction::Call;
                    ti.intrinsic_id = intrinsic_id;
                    ti.operands = call_args;

                    std::vector<uint32_t> fn_args;
                    if (intrinsic_id == 13 || intrinsic_id == 14 || intrinsic_id == 15) {
                        fn_args = call_args;
                    } else {
                        fn_args.assign(call_args.begin() + 1, call_args.end());
                    }

                    auto handleArg = [&](size_t arg, const char *prefix, const char *fallback) -> std::string {
                        if (arg >= fn_args.size()) return fallback;
                        uint32_t idx = fn_args[arg];
                        auto it = values.find(idx);
                        if (it != values.end() && it->second.role == ValueRole::BufferHandle) {
                            return it->second.resolved_name;
                        }
                        if (it != values.end() && it->second.has_expr) {
                            auto &expr = it->second.expr;
                            if (expr.find("agg(") == 0) {
                                auto parts = parseAggregateLiteral(expr);
                                if (!parts.empty()) {
                                    uint32_t lb = 0;
                                    if (parseUnsignedLiteral(parts[0], lb))
                                        return std::string(prefix) + std::to_string(lb);
                                }
                            }
                            for (auto *p : {"tex", "buf", "samp"}) {
                                if (expr.find(p) == 0) {
                                    return std::string(prefix) + expr.substr(strlen(p));
                                }
                            }
                        }
                        return fallback;
                    };

                    switch (intrinsic_id) {
                    case 57: {
                        uint32_t rc = parseUnsignedFromExpr(fn_args.size() > 0 ? fn_args[0] : 0);
                        uint32_t range = parseUnsignedFromExpr(fn_args.size() > 1 ? fn_args[1] : 0);
                        std::string name = std::string(bindingPrefixForClass(rc)) + std::to_string(range);
                        setExpr(vc, name);
                        setName(vc, name);
                        setRole(vc, rc == 3 ? ValueRole::SamplerHandle :
                                    (rc == 0 || rc == 1) ? ValueRole::TextureHandle :
                                    ValueRole::BufferHandle);
                        setBinding(vc, range);
                        setType(vc, inst.type_id);
                        ti.result_role = getRole(vc);
                        ti.result_binding = range;
                        ti.result_type = getType(vc);
                        ti.raw_expr = name;
                        break;
                    }
                    case 58: case 59: {
                        auto handle = handleArg(0, "buf", "buf0");
                        last_buffer_handle = handle;
                        uint32_t reg_idx_val = fn_args.size() > 1 ? fn_args[1] : 0;
                        auto reg_expr = getExpr(reg_idx_val);
                        if (reg_expr.find("agg(") == 0) {
                            auto parts = parseAggregateLiteral(reg_expr);
                            if (!parts.empty()) reg_expr = parts[0];
                        }
                        std::string load_expr = "(reinterpret_cast<device float4&>(" + handle + "[(" + reg_expr + ")*64]))";
                        setExpr(vc, load_expr);
                        setType(vc, inst.type_id);
                        ti.result_type = resolveType(inst.type_id, module);
                        ti.raw_expr = load_expr;
                        break;
                    }
                    case 68: {
                        auto handle = handleArg(0, "buf", "buf0");
                        last_buffer_handle = handle;
                        uint32_t idx_val = fn_args.size() > 1 ? fn_args[1] : 0;
                        auto idx_expr = getExpr(idx_val);
                        if (idx_expr.find("agg(") == 0) {
                            auto parts = parseAggregateLiteral(idx_expr);
                            if (!parts.empty()) idx_expr = parts[0];
                        }
                        std::string load_expr = "(reinterpret_cast<device float4&>(" + handle + "[(" + idx_expr + ")*16]))";
                        setExpr(vc, load_expr);
                        setType(vc, inst.type_id);
                        ti.result_type = resolveType(inst.type_id, module);
                        ti.raw_expr = load_expr;
                        break;
                    }
                    case 93: {
                        uint32_t comp = parseUnsignedFromExpr(fn_args.size() > 0 ? fn_args[0] : 0);
                        std::string expr = comp == 0 ? "(int)dtid.x" : comp == 1 ? "(int)dtid.y" : "(int)dtid.z";
                        setExpr(vc, expr);
                        setRole(vc, ValueRole::ThreadID);
                        setType(vc, inst.type_id);
                        ti.result_type = resolveType(inst.type_id, module);
                        ti.result_role = ValueRole::ThreadID;
                        ti.raw_expr = expr;
                        break;
                    }
                    case 94: {
                        uint32_t comp = parseUnsignedFromExpr(fn_args.size() > 0 ? fn_args[0] : 0);
                        std::string expr = comp == 0 ? "(int)ggid.x" : comp == 1 ? "(int)ggid.y" : "(int)ggid.z";
                        setExpr(vc, expr);
                        setRole(vc, ValueRole::GroupID);
                        setType(vc, inst.type_id);
                        ti.result_type = resolveType(inst.type_id, module);
                        ti.result_role = ValueRole::GroupID;
                        ti.raw_expr = expr;
                        break;
                    }
                    case 95: {
                        uint32_t comp = parseUnsignedFromExpr(fn_args.size() > 0 ? fn_args[0] : 0);
                        std::string expr = comp == 0 ? "(int)gtid.x" : comp == 1 ? "(int)gtid.y" : "(int)gtid.z";
                        setExpr(vc, expr);
                        setRole(vc, ValueRole::GroupThreadID);
                        setType(vc, inst.type_id);
                        ti.result_type = resolveType(inst.type_id, module);
                        ti.result_role = ValueRole::GroupThreadID;
                        ti.raw_expr = expr;
                        break;
                    }
                    case 96: {
                        std::string expr = "(int)(gtid.x + gtid.y * gsz.x + gtid.z * gsz.x * gsz.y)";
                        setExpr(vc, expr);
                        setRole(vc, ValueRole::GroupThreadID);
                        setType(vc, inst.type_id);
                        ti.result_type = resolveType(inst.type_id, module);
                        ti.result_role = ValueRole::GroupThreadID;
                        ti.raw_expr = expr;
                        break;
                    }
                    default:
                        ti.raw_expr = "0";
                        break;
                    }
                } else {
                    ti.kind = TypedInstruction::Call;
                    ti.operands = call_args;
                    ti.raw_expr = "0";
                    setExpr(vc, emitValue(vc));
                    setType(vc, inst.type_id);
                    ti.result_type = resolveType(inst.type_id, module);
                }
                break;
            }

            case LLVMInstruction::ExtractValue: {
                ti.kind = TypedInstruction::ExtractValue;
                ti.operands = inst.operands;

                if (inst.operands.size() >= 2) {
                    uint32_t agg_id = inst.operands[0];
                    uint32_t field_idx = inst.operands[1];
                    MSLType agg_type = getType(agg_id);
                    auto agg_expr = getExpr(agg_id);

                    uint32_t agg_type_id = 0;
                    auto agg_it = values.find(agg_id);
                    if (agg_it != values.end()) {
                        if (inst.operands[0] < module.types.size())
                            agg_type_id = module.types[inst.operands[0]].kind == LLVMType::Struct ? 1 : 0;
                    }

                    bool is_struct = (agg_type.kind == MSLTypeKind::Struct);

                    if (is_struct && field_idx < agg_type.struct_fields.size()) {
                        MSLType field_type = agg_type.struct_fields[field_idx];
                        ti.result_type = field_type;
                        setType(vc, inst.type_id > 0 ? inst.type_id : 0);
                        if (field_idx == 0) {
                            setExpr(vc, agg_expr);
                        } else {
                            setExpr(vc, "0");
                        }
                    } else if (isVectorType(agg_type) && field_idx < 4) {
                        MSLType field_type = scalarType(agg_type);
                        ti.result_type = field_type;
                        const char *suffix[] = {".x", ".y", ".z", ".w"};
                        std::string extract_expr = "(" + agg_expr + ")" + suffix[field_idx];
                        setExpr(vc, extract_expr);
                        setType(vc, inst.type_id > 0 ? inst.type_id : 0);
                    } else {
                        ti.result_type = resolveType(inst.type_id, module);
                        setExpr(vc, "0");
                        setType(vc, inst.type_id);
                    }
                }
                break;
            }

            case LLVMInstruction::InsertValue: {
                ti.kind = TypedInstruction::InsertValue;
                ti.operands = inst.operands;
                MSLType agg_type = inst.operands.size() >= 1 ? getType(inst.operands[0]) : MSLType{};
                ti.result_type = agg_type;
                setType(vc, inst.type_id);
                auto agg_expr = inst.operands.size() >= 1 ? getExpr(inst.operands[0]) : "float4(0)";
                setExpr(vc, agg_expr);
                break;
            }

            case LLVMInstruction::ExtractElement: {
                ti.kind = TypedInstruction::ExtractElement;
                ti.operands = inst.operands;
                MSLType vec_type = inst.operands.size() >= 1 ? getType(inst.operands[0]) : MSLType{};
                if (isVectorType(vec_type))
                    ti.result_type = scalarType(vec_type);
                else
                    ti.result_type = resolveType(inst.type_id, module);
                setType(vc, inst.type_id);
                auto vec_expr = inst.operands.size() >= 1 ? getExpr(inst.operands[0]) : "float4(0)";
                auto idx_expr = inst.operands.size() >= 2 ? getExpr(inst.operands[1]) : "0";
                uint32_t idx_val = 0;
                if (parseUnsignedLiteral(idx_expr, idx_val) && idx_val < 4) {
                    const char *suffix[] = {".x", ".y", ".z", ".w"};
                    setExpr(vc, vec_expr + suffix[idx_val]);
                } else {
                    setExpr(vc, vec_expr + "[" + idx_expr + "]");
                }
                break;
            }

            case LLVMInstruction::InsertElement: {
                ti.kind = TypedInstruction::InsertElement;
                ti.operands = inst.operands;
                MSLType vec_type = inst.operands.size() >= 1 ? getType(inst.operands[0]) : MSLType{};
                ti.result_type = vec_type;
                setType(vc, inst.type_id);
                auto vec_expr = inst.operands.size() >= 1 ? getExpr(inst.operands[0]) : "float4(0)";
                setExpr(vc, vec_expr);
                break;
            }

            case LLVMInstruction::ShuffleVector: {
                ti.kind = TypedInstruction::ShuffleVector;
                ti.operands = inst.operands;
                ti.result_type = resolveType(inst.type_id, module);
                setType(vc, inst.type_id);
                setExpr(vc, emitValue(vc));
                break;
            }

            case LLVMInstruction::FNeg: {
                ti.kind = TypedInstruction::UnaryOp;
                ti.operands = inst.operands;
                MSLType op_type = inst.operands.size() >= 1 ? getType(inst.operands[0]) : MSLType{};
                ti.result_type = op_type;
                setType(vc, inst.type_id);
                if (inst.operands.size() >= 1)
                    setExpr(vc, "-(" + getExpr(inst.operands[0]) + ")");
                break;
            }

            case LLVMInstruction::FAdd: case LLVMInstruction::FSub:
            case LLVMInstruction::FMul: case LLVMInstruction::FDiv:
            case LLVMInstruction::FRem:
            case LLVMInstruction::Add: case LLVMInstruction::Sub:
            case LLVMInstruction::Mul: case LLVMInstruction::UDiv:
            case LLVMInstruction::SDiv: case LLVMInstruction::URem:
            case LLVMInstruction::SRem:
            case LLVMInstruction::And: case LLVMInstruction::Or:
            case LLVMInstruction::Xor:
            case LLVMInstruction::Shl: case LLVMInstruction::LShr:
            case LLVMInstruction::AShr: {
                ti.kind = TypedInstruction::BinaryOp;
                ti.operands = inst.operands;
                MSLType lhs_type = inst.operands.size() >= 1 ? getType(inst.operands[0]) : MSLType{};
                ti.result_type = lhs_type;
                setType(vc, inst.type_id);
                if (inst.operands.size() >= 2) {
                    const char *op_str = "+";
                    switch (inst.opcode) {
                    case LLVMInstruction::Add: case LLVMInstruction::FAdd: op_str = "+"; break;
                    case LLVMInstruction::Sub: case LLVMInstruction::FSub: op_str = "-"; break;
                    case LLVMInstruction::Mul: case LLVMInstruction::FMul: op_str = "*"; break;
                    case LLVMInstruction::UDiv: case LLVMInstruction::SDiv: case LLVMInstruction::FDiv: op_str = "/"; break;
                    case LLVMInstruction::URem: case LLVMInstruction::SRem: case LLVMInstruction::FRem: op_str = "%"; break;
                    case LLVMInstruction::And: op_str = "&"; break;
                    case LLVMInstruction::Or: op_str = "|"; break;
                    case LLVMInstruction::Xor: op_str = "^"; break;
                    case LLVMInstruction::Shl: op_str = "<<"; break;
                    case LLVMInstruction::LShr: case LLVMInstruction::AShr: op_str = ">>"; break;
                    default: break;
                    }
                    setExpr(vc, "(" + getExpr(inst.operands[0]) + " " + std::string(op_str) + " " + getExpr(inst.operands[1]) + ")");
                }
                break;
            }

            case LLVMInstruction::BitCast: {
                ti.kind = TypedInstruction::CastOp;
                ti.operands = inst.operands;
                ti.result_type = resolveType(inst.type_id, module);
                setType(vc, inst.type_id);
                if (inst.operands.size() >= 1)
                    setExpr(vc, getExpr(inst.operands[0]));
                break;
            }

            case LLVMInstruction::ZExt: case LLVMInstruction::SExt:
            case LLVMInstruction::Trunc:
            case LLVMInstruction::FPToUI: case LLVMInstruction::FPToSI:
            case LLVMInstruction::UIToFP: case LLVMInstruction::SIToFP:
            case LLVMInstruction::FPTrunc: case LLVMInstruction::FPExt:
            case LLVMInstruction::PtrToInt: case LLVMInstruction::IntToPtr: {
                ti.kind = TypedInstruction::CastOp;
                ti.operands = inst.operands;
                ti.result_type = resolveType(inst.type_id, module);
                setType(vc, inst.type_id);
                if (inst.operands.size() >= 1)
                    setExpr(vc, getExpr(inst.operands[0]));
                break;
            }

            case LLVMInstruction::ICmp: case LLVMInstruction::FCmp: {
                ti.kind = TypedInstruction::CompareOp;
                ti.operands = inst.operands;
                ti.result_type = {MSLTypeKind::Bool, 0, {}};
                if (inst.opcode == LLVMInstruction::FCmp && inst.operands.size() >= 1)
                    ti.cmp_predicate = inst.operands[0];
                else if (inst.opcode == LLVMInstruction::ICmp && inst.operands.size() >= 1)
                    ti.cmp_predicate = inst.operands[0];
                setType(vc, inst.type_id);
                if (inst.operands.size() >= 3) {
                    auto lhs = getExpr(inst.operands[1]);
                    auto rhs = getExpr(inst.operands[2]);
                    uint32_t pred = inst.operands[0];
                    const char *cmp = "==";
                    if (pred == 0) cmp = "false";
                    else if (pred == 1 || pred == 8) cmp = "==";
                    else if (pred == 2 || pred == 9) cmp = ">";
                    else if (pred == 3 || pred == 10) cmp = ">=";
                    else if (pred == 4 || pred == 11) cmp = "<";
                    else if (pred == 5 || pred == 12) cmp = "<=";
                    else if (pred == 6 || pred == 13) cmp = "!=";
                    else if (pred >= 7 && pred <= 15) cmp = "true";
                    if (pred == 0)
                        setExpr(vc, "false");
                    else if (pred >= 15)
                        setExpr(vc, "true");
                    else
                        setExpr(vc, "(" + lhs + " " + std::string(cmp) + " " + rhs + ")");
                }
                break;
            }

            case LLVMInstruction::Select: {
                ti.kind = TypedInstruction::CastOp;
                ti.operands = inst.operands;
                MSLType true_type = inst.operands.size() >= 2 ? getType(inst.operands[1]) : MSLType{};
                ti.result_type = true_type;
                setType(vc, inst.type_id);
                if (inst.operands.size() >= 3) {
                    setExpr(vc, "(" + getExpr(inst.operands[0]) + " ? " +
                                 getExpr(inst.operands[1]) + " : " +
                                 getExpr(inst.operands[2]) + ")");
                }
                break;
            }

            case LLVMInstruction::PHI: {
                ti.kind = TypedInstruction::Phi;
                ti.operands = inst.operands;
                ti.result_type = resolveType(inst.type_id, module);
                setType(vc, inst.type_id);
                setExpr(vc, emitValue(vc));
                break;
            }

            case LLVMInstruction::Alloca: {
                ti.kind = TypedInstruction::Alloca;
                ti.result_type = resolveType(inst.type_id, module);
                setType(vc, inst.type_id);
                setExpr(vc, emitValue(vc));
                break;
            }

            case LLVMInstruction::Load: {
                ti.kind = TypedInstruction::Load;
                ti.operands = inst.operands;
                ti.result_type = resolveType(inst.type_id, module);
                setType(vc, inst.type_id);
                if (inst.operands.size() >= 1)
                    setExpr(vc, emitValue(vc));
                break;
            }

            case LLVMInstruction::Store: {
                ti.kind = TypedInstruction::Store;
                ti.operands = inst.operands;
                break;
            }

            case LLVMInstruction::GetElementPtr: {
                ti.kind = TypedInstruction::GEP;
                ti.operands = inst.operands;
                ti.result_type = resolveType(inst.type_id, module);
                setType(vc, inst.type_id);
                if (inst.operands.size() >= 1) {
                    std::string gep = getExpr(inst.operands[0]);
                    for (size_t i = 1; i < inst.operands.size(); i++) {
                        auto idx = getExpr(inst.operands[i]);
                        if (idx != "0" && idx != "0.0" && idx != "0.0f")
                            gep += " + " + idx;
                    }
                    setExpr(vc, gep);
                }
                break;
            }

            case LLVMInstruction::Ret: {
                ti.kind = TypedInstruction::Return;
                ti.operands = inst.operands;
                break;
            }
            case LLVMInstruction::Br: {
                ti.kind = TypedInstruction::Branch;
                ti.operands = inst.operands;
                break;
            }
            case LLVMInstruction::Switch: {
                ti.kind = TypedInstruction::Switch;
                ti.operands = inst.operands;
                break;
            }
            case LLVMInstruction::Unreachable: {
                ti.kind = TypedInstruction::Unreachable;
                break;
            }
            default: {
                ti.kind = TypedInstruction::Nop;
                ti.result_type = resolveType(inst.type_id, module);
                setType(vc, inst.type_id);
                setExpr(vc, "0");
                break;
            }
            }

            tb.instructions.push_back(std::move(ti));
            if (produces_value) vc++;
        }

        tf.blocks.push_back(std::move(tb));
    }

    tmod.entry = std::move(tf);
    return std::optional<TypedModule>(std::in_place, std::move(tmod));
}

}
