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
  // Emit the stable IRConverter-compatible ray-generation visible-function
  // ABI instead of a graphics/compute entry point.  This is used by the
  // custom HitObject provider when Metal Shader Converter cannot consume
  // DXIL 1.9 SER operations.
  bool ray_generation = false;
  // Library functions are DXIL-mangled (for example `?raygen@@...`).
  // When a caller selects one export, use this source-level name to locate
  // that function instead of relying on module order.
  std::string entry_point;
  // Each node's output handles carry a source-node tag.  The tag keeps
  // identically numbered output metadata from different nodes distinct in
  // the GPU work-graph routing table.
  uint32_t node_output_tag = 0;
  // Requires the version-4/5 routing context; replay must opt in atomically.
  bool node_routing = false;
  // Inject the bounded GPU capture ABI needed to emulate pixel
  // AttributeAtVertex on devices without Metal's vertex_value<T> support.
  // The pipeline binds a transient three-record buffer at slot 28 and only
  // enables this variant for a validated single triangle draw.
  bool attribute_at_vertex_capture = false;
  uint32_t attribute_at_vertex_input_id = UINT32_MAX;
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
  // D3D12's PSO SampleMask has no Metal render-encoder setter.  For
  // sample-frequency DXIL pixel shaders the lowerer applies it to the
  // sample-id/coverage inputs and discards masked samples before side effects.
  uint32_t sample_mask = 0xffffffffu;
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
