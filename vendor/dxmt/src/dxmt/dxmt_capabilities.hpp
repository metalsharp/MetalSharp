#pragma once

#include "Metal.hpp"
#include <cstdint>

namespace dxmt {

// A snapshot of host capabilities.  This is deliberately separate from the
// D3D12 feature report: it describes what the provider can attempt, while the
// D3D12 layer promotes a feature only after behavior evidence is available.
struct HostCapabilities {
  static constexpr uint32_t kSchemaVersion = 1;

  uint32_t schema_version = kSchemaVersion;
  WMTMetalVersion metal_version = WMTMetal310;
  uint64_t registry_id = 0;
  uint64_t recommended_max_working_set_size = 0;
  uint64_t current_allocated_size = 0;
  uint64_t max_object_threadgroups = 0;
  uint32_t texture_sample_counts_mask = 0;
  bool supports_raster_order_groups = false;
  bool supports_pull_model_interpolation = false;
  bool supports_shader_barycentrics = false;
  bool supports_programmable_sample_positions = false;

  bool device_available = false;
  bool apple_family7 = false;
  bool apple_family8 = false;
  bool apple_family9 = false;
  bool mac_family2 = false;
  bool metal3_family = false;
  bool supports_mtl4_command_queue = false;
  bool supports_shared_events = false;
  bool supports_unified_memory = false;
  bool supports_bc_texture_compression = false;
  bool supports_native_raytracing = false;
  bool supports_spatial_scaler = false;
  bool supports_temporal_scaler = false;

  // These providers are semantic fallbacks, not capability claims for any
  // particular D3D12 feature.  They are always available once a DXMT device
  // has been constructed and remain subject to per-operation proof.
  bool supports_compute_emulation = false;
  bool supports_cpu_reference = true;

  bool
  supportsTextureSampleCount(uint8_t sample_count) const {
    return sample_count < 32 && (texture_sample_counts_mask & (uint32_t(1) << sample_count)) != 0;
  }
};

HostCapabilities
ProbeHostCapabilities(const WMT::Device &device, WMTMetalVersion metal_version, uint64_t max_object_threadgroups);

const char *HostCapabilityBool(bool value);

} // namespace dxmt
