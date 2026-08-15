#pragma once

#include <cstdint>

namespace dxmt {

static constexpr uint32_t kD3D12VKD3DDirectBufferSlots = 31;
static constexpr uint32_t kD3D12VKD3DDirectFragmentTextureSlots = 16;
static constexpr uint32_t kD3D12VKD3DDirectFragmentSamplerSlots = 4;
static constexpr uint32_t kD3D12VKD3DDirectComputeTextureSlots = 8;
static constexpr uint32_t kD3D12VKD3DDirectComputeSamplerSlots = 4;

struct D3D12ShaderBindingCompletenessDesc {
  uint32_t buffer_count = 0;
  uint32_t texture_count = 0;
  uint32_t sampler_count = 0;
  uint64_t required_buffers = 0;
  uint64_t required_textures = 0;
  uint64_t required_samplers = 0;
  bool use_required_buffers = false;
  bool use_required_textures = false;
  bool use_required_samplers = false;
  uint64_t bound_buffers = 0;
  uint64_t bound_textures = 0;
  uint64_t bound_samplers = 0;
  uint64_t fallback_buffers = 0;
  uint64_t fallback_textures = 0;
  uint64_t fallback_samplers = 0;
};

struct D3D12ShaderBindingCompletenessSummary {
  uint64_t required_buffers = 0;
  uint64_t required_textures = 0;
  uint64_t required_samplers = 0;
  uint64_t missing_buffers = 0;
  uint64_t missing_textures = 0;
  uint64_t missing_samplers = 0;
  uint32_t required_buffer_count = 0;
  uint32_t required_texture_count = 0;
  uint32_t required_sampler_count = 0;
  uint32_t bound_buffer_count = 0;
  uint32_t bound_texture_count = 0;
  uint32_t bound_sampler_count = 0;
  uint32_t fallback_buffer_count = 0;
  uint32_t fallback_texture_count = 0;
  uint32_t fallback_sampler_count = 0;
};

inline uint64_t D3D12DirectBindingMask(uint32_t count) {
  if (count == 0)
    return 0;
  if (count >= 64)
    return ~0ull;
  return (1ull << count) - 1ull;
}

inline uint32_t D3D12CountBindingBits(uint64_t mask) {
  uint32_t count = 0;
  while (mask) {
    count += static_cast<uint32_t>(mask & 1ull);
    mask >>= 1;
  }
  return count;
}

inline D3D12ShaderBindingCompletenessSummary
D3D12EvaluateShaderBindingCompleteness(
    const D3D12ShaderBindingCompletenessDesc &desc) {
  D3D12ShaderBindingCompletenessSummary summary = {};
  summary.required_buffers =
      desc.use_required_buffers ? desc.required_buffers
                                : D3D12DirectBindingMask(desc.buffer_count);
  summary.required_textures =
      desc.use_required_textures ? desc.required_textures
                                 : D3D12DirectBindingMask(desc.texture_count);
  summary.required_samplers =
      desc.use_required_samplers ? desc.required_samplers
                                 : D3D12DirectBindingMask(desc.sampler_count);

  const uint64_t bound_buffers =
      desc.bound_buffers | desc.fallback_buffers;
  const uint64_t bound_textures =
      desc.bound_textures | desc.fallback_textures;
  const uint64_t bound_samplers =
      desc.bound_samplers | desc.fallback_samplers;

  summary.missing_buffers = summary.required_buffers & ~bound_buffers;
  summary.missing_textures = summary.required_textures & ~bound_textures;
  summary.missing_samplers = summary.required_samplers & ~bound_samplers;

  summary.required_buffer_count =
      D3D12CountBindingBits(summary.required_buffers);
  summary.required_texture_count =
      D3D12CountBindingBits(summary.required_textures);
  summary.required_sampler_count =
      D3D12CountBindingBits(summary.required_samplers);
  summary.bound_buffer_count =
      D3D12CountBindingBits(desc.bound_buffers & summary.required_buffers);
  summary.bound_texture_count =
      D3D12CountBindingBits(desc.bound_textures & summary.required_textures);
  summary.bound_sampler_count =
      D3D12CountBindingBits(desc.bound_samplers & summary.required_samplers);
  summary.fallback_buffer_count =
      D3D12CountBindingBits(desc.fallback_buffers & summary.required_buffers);
  summary.fallback_texture_count =
      D3D12CountBindingBits(desc.fallback_textures & summary.required_textures);
  summary.fallback_sampler_count =
      D3D12CountBindingBits(desc.fallback_samplers & summary.required_samplers);
  return summary;
}

} // namespace dxmt
