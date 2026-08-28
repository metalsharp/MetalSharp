#pragma once

#include "dxmt_capabilities.hpp"
#include <cstdint>

namespace dxmt {

enum class ProviderKind : uint32_t {
  MetalNative = 0,
  MetalEmulation = 1,
  CpuReference = 2,
  VideoToolboxCoreVideo = 3,
  DisplayCoreAnimation = 4,
  SharedMachIOSurface = 5,
  ProtectedPlatform = 6,
  Unavailable = 0xffffffffu,
};

struct ProviderRequirements {
  bool requires_native_raytracing = false;
  bool requires_mtl4 = false;
  bool requires_video = false;
  bool requires_display = false;
  bool requires_cross_process_sharing = false;
  bool requires_protected_memory = false;
  bool prefer_cpu = false;
  bool allow_emulation = true;
};

struct ProviderSelection {
  ProviderKind kind = ProviderKind::Unavailable;
  bool available = false;
  bool behavior_proof_required = true;
};

ProviderSelection SelectProvider(const HostCapabilities &capabilities, const ProviderRequirements &requirements);

const char *ProviderKindName(ProviderKind kind);

} // namespace dxmt
