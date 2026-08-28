#include "dxmt_provider.hpp"
#include "dxmt_timeline.hpp"
#include <cstdio>
#include <string>
#include <vector>

// The architecture probe exercises the provider policy without opening a
// native Metal device.  Supply the one no-op-safe destructor thunk needed by
// WMT::Reference's null-handle cleanup.
extern "C" void NSObject_release(obj_handle_t) {}

namespace {

using dxmt::HostCapabilities;
using dxmt::ProviderKind;
using dxmt::ProviderRequirements;
using dxmt::ProviderSelection;

struct Case {
    const char* name;
    HostCapabilities capabilities;
    ProviderRequirements requirements;
    ProviderKind expected_kind;
    bool expected_available;
};

bool check(const Case& test, std::string& error) {
    const ProviderSelection actual = dxmt::SelectProvider(test.capabilities, test.requirements);
    if (actual.kind != test.expected_kind || actual.available != test.expected_available) {
        char message[256];
        std::snprintf(message, sizeof(message), "%s expected=%s/%d actual=%s/%d", test.name,
                      dxmt::ProviderKindName(test.expected_kind), test.expected_available ? 1 : 0,
                      dxmt::ProviderKindName(actual.kind), actual.available ? 1 : 0);
        error = message;
        return false;
    }
    return true;
}

} // namespace

int main() {
    dxmt::CpuFence cpu_fence;
    dxmt::ExecutionTimeline timeline(cpu_fence);
    if (timeline.reserve() != 1 || timeline.reserve() != 2 || timeline.reservedValue() != 2 || timeline.valid()) {
        std::printf("{\"schema\":\"metalsharp.d3d12.provider-probe.v1\",\"pass\":false,\"error\":\"timeline "
                    "reservation/validity failed\"}\n");
        return 1;
    }
    timeline.completeCPU(2);
    if (timeline.completedValue() != 2 || timeline.canEncode(WMT::CommandBuffer{}, 1)) {
        std::printf("{\"schema\":\"metalsharp.d3d12.provider-probe.v1\",\"pass\":false,\"error\":\"timeline "
                    "completion/invalid encoding failed\"}\n");
        return 1;
    }

    HostCapabilities native;
    native.device_available = true;
    native.supports_compute_emulation = true;
    native.supports_cpu_reference = true;
    native.supports_native_raytracing = true;
    native.supports_mtl4_command_queue = true;

    HostCapabilities no_native = native;
    no_native.supports_native_raytracing = false;
    no_native.supports_mtl4_command_queue = false;

    HostCapabilities no_device;
    no_device.supports_compute_emulation = true;
    no_device.supports_cpu_reference = true;

    std::vector<Case> cases = {
        {"native_default", native, {}, ProviderKind::MetalNative, true},
        {"native_raytracing", native, ProviderRequirements{.requires_native_raytracing = true},
         ProviderKind::MetalNative, true},
        {"emulated_raytracing", no_native, ProviderRequirements{.requires_native_raytracing = true},
         ProviderKind::MetalEmulation, true},
        {"unavailable_raytracing", no_native,
         ProviderRequirements{.requires_native_raytracing = true, .allow_emulation = false}, ProviderKind::Unavailable,
         false},
        {"emulated_mtl4", no_native, ProviderRequirements{.requires_mtl4 = true}, ProviderKind::MetalEmulation, true},
        {"cpu_preference", native, ProviderRequirements{.prefer_cpu = true}, ProviderKind::CpuReference, true},
        {"no_device_emulation", no_device, {}, ProviderKind::MetalEmulation, true},
        {"no_device_cpu", no_device, ProviderRequirements{.allow_emulation = false}, ProviderKind::CpuReference, true},
        {"video_requires_real_provider", native, ProviderRequirements{.requires_video = true},
         ProviderKind::VideoToolboxCoreVideo, false},
        {"display_requires_real_provider", native, ProviderRequirements{.requires_display = true},
         ProviderKind::DisplayCoreAnimation, false},
        {"sharing_requires_real_provider", native, ProviderRequirements{.requires_cross_process_sharing = true},
         ProviderKind::SharedMachIOSurface, false},
        {"protected_requires_real_provider", native, ProviderRequirements{.requires_protected_memory = true},
         ProviderKind::ProtectedPlatform, false},
    };

    std::string error;
    for (const auto& test : cases) {
        if (!check(test, error)) {
            std::printf("{\"schema\":\"metalsharp.d3d12.provider-probe.v1\",\"pass\":false,\"error\":\"%s\"}\n",
                        error.c_str());
            return 1;
        }
    }

    std::printf("{\"schema\":\"metalsharp.d3d12.provider-probe.v1\",\"pass\":true,\"case_count\":%zu,\"timeline_case\":"
                "true,\"no_silent_fallback\":true}\n",
                cases.size());
    return 0;
}
