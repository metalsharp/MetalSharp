#include "metalsharp/HostRuntimeABI.h"

#include <cassert>
#include <cstddef>
#include <cstring>

int main() {
    MetalSharpHostAbiVersion version = metalsharp_host_abi_version();
    assert(version.magic == METALSHARP_HOST_ABI_MAGIC);
    assert(version.major == METALSHARP_HOST_ABI_VERSION_MAJOR);
    assert(version.minor == METALSHARP_HOST_ABI_VERSION_MINOR);
    assert(metalsharp_host_abi_is_compatible(&version));

    MetalSharpHostRuntimePaths paths = {};
    paths.struct_size = sizeof(paths);
    paths.bottle_id = "steam_620";
    assert(paths.struct_size >= offsetof(MetalSharpHostRuntimePaths, log_path) + sizeof(paths.log_path));

    MetalSharpSteamBridgeConfig bridge = {};
    bridge.struct_size = sizeof(bridge);
    bridge.socket_path = "/Users/fixture/.metalsharp/" METALSHARP_STEAM_BRIDGE_SOCKET_NAME;
    bridge.appid = 620;
    assert(std::strcmp(bridge.socket_path, "/Users/fixture/.metalsharp/run/steam-bridge.sock") == 0);

    MetalSharpManagedRuntimeConfig managed = {};
    managed.struct_size = sizeof(managed);
    managed.mono_lib = "@loader_path/libmonosgen-2.0.dylib";
    assert(managed.struct_size >=
           offsetof(MetalSharpManagedRuntimeConfig, mono_config_dir) + sizeof(managed.mono_config_dir));

    MetalSharpHostCapabilities capabilities = {};
    assert(metalsharp_host_self_test(&capabilities) == METALSHARP_HOST_OK);
    assert(capabilities.supports_steam_bridge == 1u);
    assert(capabilities.supports_managed_runtime == 1u);
    assert((capabilities.services & METALSHARP_HOST_SERVICE_BIT(METALSHARP_HOST_SERVICE_STEAM)) != 0u);

    return 0;
}
