#pragma once

#include "dxil_ir.hpp"
#include "dxil_container.hpp"
#include <string>
#include <sstream>
#include <optional>
#include <vector>

namespace dxmt::dxil {

struct TypedMSLShader {
    std::string source;
    std::string entry_point;
    uint32_t tg_size[3] = {1, 1, 1};
    uint32_t unsupported_intrinsics = 0;
    uint32_t unsupported_opcodes = 0;
    uint32_t typed_value_count = 0;
    uint32_t auto_value_count = 0;
    std::vector<std::string> diagnostics;
};

enum class MSLVertexTableIndexingMode : uint32_t {
    CompactBySlotMask = 0,
    RawSlot = 1,
};

struct MSLVertexInputElement {
    uint32_t shader_register = 0;
    uint32_t table_index = 0;
    uint32_t input_slot = 0;
    uint32_t aligned_byte_offset = 0;
    uint32_t dxgi_format = 0;
    uint32_t metal_format = 0;
    bool per_instance = false;
    uint32_t instance_step_rate = 1;
    MSLVertexTableIndexingMode table_indexing_mode =
        MSLVertexTableIndexingMode::CompactBySlotMask;
    bool system_value = false;
};

struct MSLLoweringOptions {
  std::vector<MSLVertexInputElement> vertex_inputs;
  bool depth_bounds_test = false;
  bool depth_bounds_multisample = false;
  bool sampler_feedback = false;
  // Counter UAVs currently use one reserved direct buffer slot and do not
  // support dynamically indexed resource heaps.
  bool resource_heap_directly_indexed = false;
  // Reserve the DXMT fragment runtime slot and carry the flat
  // SV_ShadingRate-producing value through a dedicated interface member.  A
  // command-list replay can then select the effective rate without confusing
  // the source semantic with Metal's screen-space rate map.
  bool vrs_per_primitive = false;
  // Replace ordinary point-in-triangle rasterization with the bounded
  // conservative coverage replay path for the supported reference model.
  bool conservative_rasterization = false;
};

inline uint32_t MSLResolveVertexInputTableIndex(uint32_t shader_register,
                                                const MSLLoweringOptions &options) {
    for (const auto &input : options.vertex_inputs) {
        if (!input.system_value && input.shader_register == shader_register) {
            if (input.table_indexing_mode == MSLVertexTableIndexingMode::RawSlot)
                return input.input_slot;
            return input.table_index;
        }
    }
    return shader_register < 16 ? shader_register : 0;
}

inline std::string MSLVertexPullExpression(uint32_t shader_register,
                                           const MSLLoweringOptions &options) {
    uint32_t table_index = MSLResolveVertexInputTableIndex(shader_register, options);
    uint32_t aligned_byte_offset = 0;
    uint32_t dxgi_format = 0;
    uint32_t per_instance = 0;
    uint32_t instance_step_rate = 1;
    for (const auto &input : options.vertex_inputs) {
        if (!input.system_value && input.shader_register == shader_register) {
            aligned_byte_offset = input.aligned_byte_offset;
            dxgi_format = input.dxgi_format;
            per_instance = input.per_instance ? 1u : 0u;
            instance_step_rate = input.instance_step_rate;
            break;
        }
    }
    if (table_index >= 16)
        table_index = 0;
    return "m12_load_vertex_attr(" + std::to_string(table_index) +
           ", " + std::to_string(aligned_byte_offset) +
           ", " + std::to_string(dxgi_format) +
           ", " + std::to_string(per_instance) +
           ", " + std::to_string(instance_step_rate) +
           ", vid, iid, buf16, buf" + std::to_string(table_index) +
           ", buf29, buf30)";
}

class MSLLowering {
public:
    static std::optional<TypedMSLShader> lower(const LLVMModule &module,
                                                const DxilParsedShader &shader,
                                                const MSLLoweringOptions &options = {});

private:
    static std::string emitTypedDecl(const std::string &name, const MSLType &type);
    static std::string defaultValueForType(const MSLType &type);
};

}
