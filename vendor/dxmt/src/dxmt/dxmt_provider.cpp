#include "dxmt_provider.hpp"

namespace dxmt {

ProviderSelection
SelectProvider(const HostCapabilities &capabilities, const ProviderRequirements &requirements) {
  ProviderSelection selection;

  if (requirements.requires_protected_memory) {
    selection.kind = ProviderKind::ProtectedPlatform;
    selection.available = false;
    return selection;
  }
  if (requirements.requires_video) {
    selection.kind = ProviderKind::VideoToolboxCoreVideo;
    selection.available = false;
    return selection;
  }
  if (requirements.requires_display) {
    selection.kind = ProviderKind::DisplayCoreAnimation;
    selection.available = false;
    return selection;
  }
  if (requirements.requires_cross_process_sharing) {
    selection.kind = ProviderKind::SharedMachIOSurface;
    selection.available = false;
    return selection;
  }
  if (requirements.requires_native_raytracing && !capabilities.supports_native_raytracing) {
    if (!requirements.allow_emulation || !capabilities.supports_compute_emulation) {
      selection.kind = ProviderKind::Unavailable;
      selection.available = false;
      return selection;
    }
    selection.kind = ProviderKind::MetalEmulation;
    selection.available = true;
    return selection;
  }
  if (requirements.requires_mtl4 && !capabilities.supports_mtl4_command_queue) {
    if (!requirements.allow_emulation || !capabilities.supports_compute_emulation) {
      selection.kind = ProviderKind::Unavailable;
      selection.available = false;
      return selection;
    }
    selection.kind = ProviderKind::MetalEmulation;
    selection.available = true;
    return selection;
  }
  if (requirements.prefer_cpu) {
    selection.kind = ProviderKind::CpuReference;
    selection.available = capabilities.supports_cpu_reference;
    return selection;
  }
  if (capabilities.device_available) {
    selection.kind = ProviderKind::MetalNative;
    selection.available = true;
    return selection;
  }
  if (requirements.allow_emulation && capabilities.supports_compute_emulation) {
    selection.kind = ProviderKind::MetalEmulation;
    selection.available = true;
    return selection;
  }
  selection.kind = ProviderKind::CpuReference;
  selection.available = capabilities.supports_cpu_reference;
  return selection;
}

const char *
ProviderKindName(ProviderKind kind) {
  switch (kind) {
  case ProviderKind::MetalNative:
    return "metal-native";
  case ProviderKind::MetalEmulation:
    return "metal-emulation";
  case ProviderKind::CpuReference:
    return "cpu-reference";
  case ProviderKind::VideoToolboxCoreVideo:
    return "videotoolbox-corevideo";
  case ProviderKind::DisplayCoreAnimation:
    return "display-coreanimation";
  case ProviderKind::SharedMachIOSurface:
    return "shared-mach-iosurface";
  case ProviderKind::ProtectedPlatform:
    return "protected-platform";
  case ProviderKind::Unavailable:
    return "unavailable";
  }
  return "unknown";
}

} // namespace dxmt
