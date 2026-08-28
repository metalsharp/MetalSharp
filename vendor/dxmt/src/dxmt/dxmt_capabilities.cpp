#include "dxmt_capabilities.hpp"

namespace dxmt {

HostCapabilities
ProbeHostCapabilities(const WMT::Device &device, WMTMetalVersion metal_version, uint64_t max_object_threadgroups) {
  HostCapabilities capabilities;
  capabilities.metal_version = metal_version;
  capabilities.max_object_threadgroups = max_object_threadgroups;
  capabilities.device_available = device.handle != NULL_OBJECT_HANDLE;
  if (!capabilities.device_available)
    return capabilities;

  capabilities.registry_id = device.registryID();
  capabilities.recommended_max_working_set_size = device.recommendedMaxWorkingSetSize();
  capabilities.current_allocated_size = device.currentAllocatedSize();
  capabilities.apple_family7 = device.supportsFamily(WMTGPUFamilyApple7);
  capabilities.apple_family8 = device.supportsFamily(WMTGPUFamilyApple8);
  capabilities.apple_family9 = device.supportsFamily(WMTGPUFamilyApple9);
  capabilities.mac_family2 = device.supportsFamily(WMTGPUFamilyMac2);
  capabilities.metal3_family = device.supportsFamily(WMTGPUFamilyMetal3);
  capabilities.supports_shared_events = device.newSharedEvent().handle != 0;
  capabilities.supports_mtl4_command_queue = device.newMTL4CommandQueue().handle != 0;
  capabilities.supports_unified_memory = device.hasUnifiedMemory();
  capabilities.supports_bc_texture_compression = device.supportsBCTextureCompression();
  capabilities.supports_native_raytracing = device.supportsRaytracing();
  capabilities.supports_spatial_scaler = device.supportsFXSpatialScaler();
  capabilities.supports_temporal_scaler = device.supportsFXTemporalScaler();
  capabilities.supports_compute_emulation = true;

  // Query the complete sample-count range exposed by Metal rather than
  // inferring it from the GPU family.  The bit is indexed by sample count.
  for (uint8_t sample_count = 1; sample_count <= 16; sample_count++) {
    if (device.supportsTextureSampleCount(sample_count))
      capabilities.texture_sample_counts_mask |= uint32_t(1) << sample_count;
  }

  return capabilities;
}

const char *
HostCapabilityBool(bool value) {
  return value ? "true" : "false";
}

} // namespace dxmt
