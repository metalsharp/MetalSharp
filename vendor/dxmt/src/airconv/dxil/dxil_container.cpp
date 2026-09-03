#include "dxil_container.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

namespace dxmt::dxil {

namespace {

static uint32_t read_u32(const uint8_t *data) {
  uint32_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

static void parse_signature(const uint8_t *data, size_t size, bool input,
                            DxilParsedShader &shader) {
  // ISG1/OSG1 signature parts start with DxilProgramSignature.  The
  // serialized element is 0x20 bytes and its SystemValue and Register fields
  // are at offsets 0x0c and 0x14 respectively.
  if (!data || size < 8)
    return;
  const uint32_t parameter_count = read_u32(data);
  const uint32_t parameter_offset = read_u32(data + 4);
  if (parameter_count > 128 || parameter_offset > size ||
      uint64_t(parameter_offset) + uint64_t(parameter_count) * 0x20 > size)
    return;
  for (uint32_t i = 0; i < parameter_count; ++i) {
    const uint8_t *element = data + parameter_offset + uint64_t(i) * 0x20;
    const uint32_t system_value = read_u32(element + 0x0c);
    const int32_t register_index =
        static_cast<int32_t>(read_u32(element + 0x14));
    if (system_value == 24) { // DxilProgramSigSemantic::ShadingRate
      if (input)
        shader.shading_rate_input_register = register_index;
      else {
        shader.shading_rate_output_register = register_index;
        shader.shading_rate_output_id = static_cast<int32_t>(i);
      }
    } else if (system_value == 5) {
      // DxilProgramSigSemantic::ViewportArrayIndex.
      if (input)
        shader.viewport_index_input_register = register_index;
      else {
        shader.viewport_index_output_register = register_index;
        shader.viewport_index_output_id = static_cast<int32_t>(i);
      }
    } else if (system_value == 4) {
      // DxilProgramSigSemantic::RenderTargetArrayIndex.
      if (input)
        shader.render_target_array_index_input_register = register_index;
      else {
        shader.render_target_array_index_output_register = register_index;
        shader.render_target_array_index_output_id = static_cast<int32_t>(i);
      }
    } else if (system_value == 23 && input) {
      // DxilProgramSigSemantic::Barycentrics.  The PSV0 record supplies the
      // interpolation mode used when emitting the Metal builtin attribute.
      shader.barycentrics_input_id = static_cast<int32_t>(i);
    }
  }
}

static void parse_psv0(const uint8_t *data, size_t size,
                       DxilParsedShader &shader) {
  // PSV0 is versioned by the serialized runtime-info size.  We only need the
  // input signature records here, but walk the preceding resource/string
  // tables with their serialized sizes so malformed or future blobs fail
  // closed instead of being interpreted at fixed offsets.
  if (!data || size < 8)
    return;
  const uint32_t runtime_size = read_u32(data);
  if (runtime_size < 36 || runtime_size > size - 8)
    return;
  const uint8_t *runtime = data + 4;
  size_t offset = 4 + runtime_size;
  if (offset + 4 > size)
    return;
  const uint32_t resource_count = read_u32(data + offset);
  offset += 4;
  if (resource_count > 4096)
    return;
  if (resource_count) {
    if (offset + 4 > size)
      return;
    const uint32_t resource_size = read_u32(data + offset);
    offset += 4;
    if (resource_size < 16 || resource_size > size ||
        uint64_t(offset) + uint64_t(resource_count) * resource_size > size)
      return;
    offset += uint64_t(resource_count) * resource_size;
  }

  // PSVRuntimeInfo1 fields are stable through later versions.  The signature
  // counts are relative to the start of the runtime-info structure.
  const uint32_t input_elements = runtime[28];
  const uint32_t output_elements = runtime[29];
  const uint32_t patch_elements = runtime[30];
  if (offset + 4 > size)
    return;
  const uint32_t string_size = read_u32(data + offset);
  offset += 4;
  if (string_size > size || uint64_t(offset) + string_size > size)
    return;
  offset += string_size;
  if (offset + 4 > size)
    return;
  const uint32_t semantic_index_entries = read_u32(data + offset);
  offset += 4;
  if (semantic_index_entries > (size - offset) / 4)
    return;
  offset += uint64_t(semantic_index_entries) * 4;

  if (!input_elements && !output_elements && !patch_elements)
    return;
  if (offset + 4 > size)
    return;
  const uint32_t element_size = read_u32(data + offset);
  offset += 4;
  if (element_size < 14 || element_size > size)
    return;
  const uint64_t input_bytes = uint64_t(input_elements) * element_size;
  const uint64_t output_bytes = uint64_t(output_elements) * element_size;
  const uint64_t patch_bytes = uint64_t(patch_elements) * element_size;
  if (uint64_t(offset) + input_bytes + output_bytes + patch_bytes > size)
    return;

  // PSVSignatureElement0::SemanticKind is byte 11 and InterpolationMode is
  // byte 13.  Barycentrics is semantic kind 28 (the DXIL enum value).
  for (uint32_t i = 0; i < input_elements; ++i) {
    const uint8_t *element = data + offset + uint64_t(i) * element_size;
    if (element[11] == 28) {
      shader.barycentrics_interpolation = element[13];
      break;
    }
  }
}

static void parse_signatures(const uint8_t *container, size_t size,
                             DxilParsedShader &shader) {
  if (!container || size < 32 || std::memcmp(container, "DXBC", 4) != 0)
    return;
  const uint32_t container_size = read_u32(container + 24);
  const uint32_t part_count = read_u32(container + 28);
  if (container_size > size || part_count > 128 ||
      uint64_t(32) + uint64_t(part_count) * 4 > size)
    return;
  for (uint32_t i = 0; i < part_count; ++i) {
    const uint32_t part_offset = read_u32(container + 32 + i * 4);
    if (part_offset > size || uint64_t(part_offset) + 8 > size)
      continue;
    const uint32_t part_size = read_u32(container + part_offset + 4);
    if (uint64_t(part_offset) + 8 + part_size > size)
      continue;
    const uint8_t *part_data = container + part_offset + 8;
    if (std::memcmp(container + part_offset, "ISG1", 4) == 0)
      parse_signature(part_data, part_size, true, shader);
    else if (std::memcmp(container + part_offset, "OSG1", 4) == 0)
      parse_signature(part_data, part_size, false, shader);
    else if (std::memcmp(container + part_offset, "PSV0", 4) == 0)
      parse_psv0(part_data, part_size, shader);
  }
}

} // namespace

std::optional<DXILContainer> DXILContainer::parse(const void *data, size_t size) {
  if (!data || size < 16)
    return std::nullopt;

  auto *base = static_cast<const uint8_t *>(data);

  const uint32_t *vals = reinterpret_cast<const uint32_t *>(base);
  uint32_t program_version = vals[0];
  uint32_t prog_size = vals[1];

  const uint32_t *dxil_fields = vals + 2;
  uint32_t dxil_magic = dxil_fields[0];

  if (dxil_magic != DXIL_FOURCC)
    return std::nullopt;

  uint16_t dxil_minor = *reinterpret_cast<const uint16_t *>(base + 12);
  uint16_t dxil_major = *reinterpret_cast<const uint16_t *>(base + 14);
  (void)dxil_major;
  (void)dxil_minor;

  uint32_t bitcode_offset = *reinterpret_cast<const uint32_t *>(base + 16);
  uint32_t bitcode_size = *reinterpret_cast<const uint32_t *>(base + 20);

  FILE *_dbg = dxmt_dxil_open_trace_log("dxmt-dxil-trace.log");
  if (_dbg) {
    fprintf(_dbg, "DXILContainer: ver=0x%08x prog_size=%u dxil_magic=0x%08x bc_off=%u bc_sz=%u blob_size=%zu\n",
      program_version, prog_size, dxil_magic, bitcode_offset, bitcode_size, size);
    fclose(_dbg);
  }

  uint32_t dxil_magic_offset = 8;
  uint32_t actual_bitcode_start = dxil_magic_offset + bitcode_offset;

  uint32_t kind_val = (program_version >> 16) & 0xFFFF;
  DxilShaderKind kind = static_cast<DxilShaderKind>(kind_val);

  DxilShaderModel sm;
  sm.major = (program_version >> 4) & 0xF;
  sm.minor = program_version & 0xF;

  if (actual_bitcode_start >= size)
    return std::nullopt;

  if (bitcode_size == 0 || actual_bitcode_start + bitcode_size > size)
    bitcode_size = size - actual_bitcode_start;

  const uint8_t *bitcode_ptr = base + actual_bitcode_start;

  DXILContainer result;
  result.m_shader.kind = kind;
  result.m_shader.shader_model = sm;
  result.m_shader.bitcode.data = bitcode_ptr;
  result.m_shader.bitcode.size = bitcode_size;

  switch (kind) {
  case DxilShaderKind::Compute: result.m_shader.entry_point = "cs_main"; break;
  case DxilShaderKind::Vertex: result.m_shader.entry_point = "vs_main"; break;
  case DxilShaderKind::Pixel: result.m_shader.entry_point = "ps_main"; break;
  case DxilShaderKind::Geometry: result.m_shader.entry_point = "gs_main"; break;
  case DxilShaderKind::Hull: result.m_shader.entry_point = "hs_main"; break;
  case DxilShaderKind::Domain: result.m_shader.entry_point = "ds_main"; break;
  default: result.m_shader.entry_point = "main"; break;
  }

  return result;
}

void DXILContainer::annotateSignatures(const void *container, size_t size) {
  parse_signatures(static_cast<const uint8_t *>(container), size, m_shader);
}

}
