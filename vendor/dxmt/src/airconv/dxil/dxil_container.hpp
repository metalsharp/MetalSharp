#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <optional>

namespace dxmt::dxil {

static constexpr uint32_t DXIL_FOURCC = 
  (uint32_t)'D' | ((uint32_t)'X' << 8) | ((uint32_t)'I' << 16) | ((uint32_t)'L' << 24);

static constexpr uint32_t DXBC_FOURCC = 
  (uint32_t)'D' | ((uint32_t)'X' << 8) | ((uint32_t)'B' << 16) | ((uint32_t)'C' << 24);

struct DXBCChunkHeader {
  uint32_t fourCC;
  uint32_t size;
};

struct DxilProgramHeader {
  uint32_t version;
  uint32_t size;
  uint16_t dxil_major;
  uint16_t dxil_minor;
};

struct DxilShaderModel {
  uint8_t major;
  uint8_t minor;
};

enum class DxilShaderKind : uint32_t {
  Pixel = 0,
  Vertex = 1,
  Geometry = 2,
  Hull = 3,
  Domain = 4,
  Compute = 5,
  Library = 6,
  RayGeneration = 7,
  Intersection = 8,
  AnyHit = 9,
  ClosestHit = 10,
  Miss = 11,
  Callable = 12,
  Mesh = 13,
  Amplification = 14,
  Invalid = 0xFFFFFFFF,
};

struct DxilHeader {
  DxilProgramHeader program;
  uint32_t dxil_version;
  uint32_t bitcode_offset;
};

struct DxilBitcodeRef {
  const uint8_t *data;
  uint32_t size;
};

struct DxilParsedShader {
  DxilShaderKind kind;
  DxilShaderModel shader_model;
  DxilBitcodeRef bitcode;
  std::string entry_point;
  int32_t shading_rate_input_register = -1;
  int32_t shading_rate_output_register = -1;
};

class DXILContainer {
public:
  static std::optional<DXILContainer> parse(const void *data, size_t size);

  // Signature parts live in the outer DXBC container, while callers often
  // pass only the DXIL part to parse().  Annotate a parsed shader with the
  // system-value registers when the original container is available.
  void annotateSignatures(const void *container, size_t size);

  const DxilParsedShader &shader() const { return m_shader; }

private:
  DXILContainer() = default;
  DxilParsedShader m_shader;
  std::vector<uint8_t> m_storage;
};

}
