# Host Runtime ABI
**Updated:** 2026-09-22

The Host Runtime ABI is the supported boundary between Windows-facing shims, Wine unixlib modules, and macOS host services. Header: `include/metalsharp/HostRuntimeABI.h`.

## Services

The ABI owns low-level host capabilities shared by launch routes:

- process, environment, and path identity for a bottle
- runtime logging and diagnostics paths
- Steam identity bridge configuration
- managed runtime configuration for Mono/.NET launchers
- graphics, audio, and input dispatch capability reporting

Wine Steam owns Steam account/session lifecycle; the ABI injects bottle-specific runtime state into game and installer processes.

## Contract Header

```text
METALSHARP_HOST_ABI_VERSION_MAJOR
METALSHARP_HOST_ABI_VERSION_MINOR
MetalSharpHostRuntimePaths
MetalSharpSteamBridgeConfig
MetalSharpManagedRuntimeConfig
MetalSharpHostCapabilities
metalsharp_host_get_abi_version()
metalsharp_host_query_capabilities()
metalsharp_host_self_test()
```

Every struct starts with `struct_size`, so newer hosts can append fields without breaking older shims. Callers reject incompatible major versions and tolerate larger struct sizes.

Shims configure themselves through environment variables instead of machine-local paths: `METALSHARP_MONO_LIB`, `METALSHARP_MONO_ROOT`, `METALSHARP_MONO_ASSEMBLY_DIR`, `METALSHARP_MONO_CONFIG_DIR` (with `METALSHARP_HOME`/`HOME` fallbacks), and `METALSHARP_STEAM_BRIDGE_PORT`. The backend exposes `GET /runtime/host-abi` for inspecting the current ABI version, service list, bridge port, and managed runtime environment contract.

## Bottle Manifest Mapping

| Manifest field | ABI target |
|---|---|
| `id` | `MetalSharpHostRuntimePaths.bottle_id` |
| `prefix_path` | `MetalSharpHostRuntimePaths.bottle_prefix` |
| `game_install_path` | `MetalSharpHostRuntimePaths.game_install_path` |
| bottle log path | `MetalSharpHostRuntimePaths.log_path` |
| Steam appid | `MetalSharpSteamBridgeConfig.appid` |
| bridge port | `MetalSharpSteamBridgeConfig.port` |
| Mono root/lib dirs | `MetalSharpManagedRuntimeConfig` |

## Self-Test

`tests/test_host_runtime_abi.cpp` compiles the header and validates version constants, struct sizing, capabilities, and default Steam bridge configuration, so CI catches ABI breakage before runtime packaging depends on it.

## Packaging

The shared host runtime target is `metalsharp_host_runtime`, built as `libmetalsharp_host_runtime.dylib` on macOS. `tools/package/create-host-runtime.sh` stages the dylib, `HostRuntimeABI.h`, and `manifest.json` into `app/native/host/`, which Electron packages as `runtime/host/`. Setup copies those assets into `~/.metalsharp/runtime/host/`; the setup dependency check requires them, and runtime migration schema `2` treats a missing host ABI install as a repair condition.
