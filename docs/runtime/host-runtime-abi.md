# MetalSharp Host Runtime ABI
**Updated:** 2026-07-08


Status: Phase 1 draft implementation

This document defines the first supported boundary between Windows-facing shims, Wine unixlib modules, and macOS host services. The goal is to turn today's scattered dylibs and per-game shims into a bottle-aware runtime contract.

## What The ABI Owns

Host Runtime ABI services are low-level host capabilities that many launch routes need:

- process, environment, and path identity for a bottle
- runtime logging and diagnostics paths
- Steam identity bridge configuration
- managed runtime configuration for Mono/.NET launchers
- graphics, audio, and input dispatch capability reporting

The ABI does not own Steam account/session lifecycle. Wine Steam remains the long-lived Steam entity. MetalSharp owns route authority and injects bottle-specific runtime state into the game process or installer process.

## Contract Header

The C ABI lives in `include/metalsharp/HostRuntimeABI.h`. It provides:

- `METALSHARP_HOST_ABI_VERSION_MAJOR`
- `METALSHARP_HOST_ABI_VERSION_MINOR`
- `MetalSharpHostRuntimePaths`
- `MetalSharpSteamBridgeConfig`
- `MetalSharpManagedRuntimeConfig`
- `MetalSharpHostCapabilities`
- `metalsharp_host_get_abi_version()`
- `metalsharp_host_query_capabilities()`
- `metalsharp_host_self_test()`

Every struct starts with `struct_size` so newer hosts can append fields without breaking older shims. Callers must reject incompatible major versions and tolerate larger struct sizes.

## Phase 1 Runtime Changes

The initial implementation removes two brittle assumptions from the runtime:

- `src/wine/mscoree_unix.c` no longer uses a machine-local absolute Mono path. It accepts `METALSHARP_MONO_LIB`, `METALSHARP_MONO_ROOT`, `METALSHARP_MONO_ASSEMBLY_DIR`, and `METALSHARP_MONO_CONFIG_DIR`, with portable `METALSHARP_HOME`/`HOME` fallbacks.
- `src/fna/shims/steam_shim.c` no longer hardcodes the Steam bridge port only in native code. It accepts `METALSHARP_STEAM_BRIDGE_PORT`, and the Rust launcher reports/passes the same value.
- The backend exposes `GET /runtime/host-abi` so the app can inspect the current ABI version, service list, bridge port, and managed runtime environment contract.

These are small changes, but they are the necessary shape: bottle manifests and installer runtime profiles can now configure shims through explicit runtime state instead of requiring patched binaries or one global machine assumption.

### Managed PE/unixlib launch contract

The Wine `mscoree` shim keeps the managed executable identity explicit across
the PE/unix boundary. `_CorExeMain` fills `mscoree_cor_exe_main_params` with
the Unix-form executable path and directory, passes that structure to
`MSCOREE_FUNC_COR_EXE_MAIN`, and exits with the `exit_code` written by the Unix
handler. The Unix handler must not infer the executable from `_` or
`/proc/self/cmdline`; those are not portable on macOS. A failed bridge call
leaves a non-zero exit code so a managed launch cannot report success without
executing its assembly.

## Bottle Manifest Mapping

Future bottle manifests should map directly to ABI structs:

| Manifest field | ABI target |
| --- | --- |
| `id` | `MetalSharpHostRuntimePaths.bottle_id` |
| `prefix_path` | `MetalSharpHostRuntimePaths.bottle_prefix` |
| `game_install_path` | `MetalSharpHostRuntimePaths.game_install_path` |
| bottle log path | `MetalSharpHostRuntimePaths.log_path` |
| Steam appid | `MetalSharpSteamBridgeConfig.appid` |
| bridge port | `MetalSharpSteamBridgeConfig.port` |
| Mono root/lib dirs | `MetalSharpManagedRuntimeConfig` |

## Self-Test

`tests/test_host_runtime_abi.cpp` compiles the header and calls the runtime query/self-test surface from `src/runtime/host/HostRuntimeABI.cpp`. It validates version constants, struct sizing, capabilities, and default Steam bridge configuration. It is intentionally lightweight so CI can catch ABI breakage before runtime packaging work depends on it.

## Packaging

The shared host runtime target is `metalsharp_host_runtime`. macOS builds produce `libmetalsharp_host_runtime.dylib`.
On Apple Silicon it is an arm64 host artifact; it is not part of the
x86_64 Wine/Rosetta shim lane. See the [macOS artifact architecture
matrix](../architecture/macos-artifact-matrix.md) for the complete boundary.

`tools/package/create-host-runtime.sh` stages the package into `app/native/host/`:

- `libmetalsharp_host_runtime.dylib` or platform equivalent
- `HostRuntimeABI.h`
- `manifest.json`

Electron packages this directory as `runtime/host/`. This gives the app, updater, and future runtime installer a stable place to discover host ABI artifacts without scraping the native library root.

During setup, the backend copies packaged `runtime/host/` assets into:

```text
~/.metalsharp/runtime/host/
```

The setup dependency check treats the host runtime as required. Runtime migration schema `2` also treats a missing host ABI install as a repair condition so existing installs can be refreshed cleanly.

## Next Phase Hooks

Phase 2 should use this ABI boundary to generate a per-bottle runtime env block from bottle manifests. That block should be applied to installer launches, direct game launches, and Steam game handoffs that create a child process outside the already-running Steam client.
