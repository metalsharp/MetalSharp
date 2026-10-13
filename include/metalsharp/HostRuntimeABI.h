/// @file HostRuntimeABI.h
/// @brief Versioned C ABI shared by MetalSharp PE shims, Wine unixlibs, and host runtime services.
#ifndef METALSHARP_HOST_RUNTIME_ABI_H
#define METALSHARP_HOST_RUNTIME_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define METALSHARP_HOST_ABI_VERSION_MAJOR 2u
#define METALSHARP_HOST_ABI_VERSION_MINOR 0u
#define METALSHARP_HOST_ABI_MAGIC         0x4d534142u
/* The Steam bridge listens on <METALSHARP_HOME>/run/steam-bridge.sock, an
 * owner-only Unix socket (never a TCP port). */
#define METALSHARP_STEAM_BRIDGE_SOCKET_NAME  "run/steam-bridge.sock"
#define METALSHARP_HOST_SERVICE_BIT(service) (1u << (uint32_t)(service))

#if defined(__GNUC__) || defined(__clang__)
#define METALSHARP_HOST_UNUSED __attribute__((unused))
#else
#define METALSHARP_HOST_UNUSED
#endif

typedef uint32_t MetalSharpHostService;
enum {
    METALSHARP_HOST_SERVICE_PROCESS = 1u,
    METALSHARP_HOST_SERVICE_PATHS = 2u,
    METALSHARP_HOST_SERVICE_LOGGING = 3u,
    METALSHARP_HOST_SERVICE_STEAM = 4u,
    METALSHARP_HOST_SERVICE_GRAPHICS = 5u,
    METALSHARP_HOST_SERVICE_AUDIO = 6u,
    METALSHARP_HOST_SERVICE_INPUT = 7u,
    METALSHARP_HOST_SERVICE_MANAGED_RUNTIME = 8u,
};

typedef int32_t MetalSharpHostResult;
enum {
    METALSHARP_HOST_OK = 0,
    METALSHARP_HOST_ERR_UNSUPPORTED = -1,
    METALSHARP_HOST_ERR_INVALID_ARGUMENT = -2,
    METALSHARP_HOST_ERR_RUNTIME_MISSING = -3,
    METALSHARP_HOST_ERR_BRIDGE_UNAVAILABLE = -4,
    METALSHARP_HOST_ERR_BUFFER_TOO_SMALL = -5,
};

typedef struct MetalSharpHostAbiVersion {
    uint32_t magic;
    uint16_t major;
    uint16_t minor;
    uint32_t struct_size;
} MetalSharpHostAbiVersion;

typedef struct MetalSharpHostRuntimePaths {
    uint32_t struct_size;
    const char* metalsharp_home;
    const char* wine_runtime_root;
    const char* bottle_id;
    const char* bottle_prefix;
    const char* game_install_path;
    const char* log_path;
} MetalSharpHostRuntimePaths;

typedef struct MetalSharpSteamBridgeConfig {
    uint32_t struct_size;
    const char* socket_path;
    uint32_t appid;
    const char* bottle_id;
} MetalSharpSteamBridgeConfig;

typedef struct MetalSharpManagedRuntimeConfig {
    uint32_t struct_size;
    const char* mono_lib;
    const char* mono_root;
    const char* mono_assembly_dir;
    const char* mono_config_dir;
} MetalSharpManagedRuntimeConfig;

typedef struct MetalSharpHostCapabilities {
    uint32_t struct_size;
    uint32_t services;
    uint32_t supports_wow64;
    uint32_t supports_steam_bridge;
    uint32_t supports_managed_runtime;
    uint32_t supports_metal_graphics;
} MetalSharpHostCapabilities;

typedef MetalSharpHostResult (*MetalSharpHostDispatchFn)(MetalSharpHostService service, uint32_t command, void* params,
                                                         uint32_t params_size);

MetalSharpHostAbiVersion metalsharp_host_get_abi_version(void);
MetalSharpHostResult metalsharp_host_query_capabilities(MetalSharpHostCapabilities* capabilities);
MetalSharpHostResult metalsharp_host_self_test(MetalSharpHostCapabilities* capabilities);

static inline METALSHARP_HOST_UNUSED MetalSharpHostAbiVersion metalsharp_host_abi_version(void) {
    MetalSharpHostAbiVersion version = {
        METALSHARP_HOST_ABI_MAGIC,
        METALSHARP_HOST_ABI_VERSION_MAJOR,
        METALSHARP_HOST_ABI_VERSION_MINOR,
        (uint32_t)sizeof(MetalSharpHostAbiVersion),
    };
    return version;
}

static inline METALSHARP_HOST_UNUSED int metalsharp_host_abi_is_compatible(const MetalSharpHostAbiVersion* version) {
    return version && version->magic == METALSHARP_HOST_ABI_MAGIC &&
           version->major == METALSHARP_HOST_ABI_VERSION_MAJOR &&
           version->struct_size >= sizeof(MetalSharpHostAbiVersion);
}

#ifdef __cplusplus
}
#endif

#endif
