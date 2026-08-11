# macOS artifact architecture matrix
**Updated:** 2026-08-11

MetalSharp ships one arm64 macOS application while its Wine-facing native
artifacts remain x86_64 for Rosetta-backed Windows execution. The architecture
boundary is explicit in `CMakeLists.txt` and is validated after native builds
and before packaging.

| Artifact | Architecture | Process boundary |
| --- | --- | --- |
| Electron app and Rust backend | arm64 | Native macOS application |
| `metalsharp_launcher` | arm64 | Native helper that starts MetalSharp Wine |
| `metalsharp_host_runtime` and `MetalSharpMigrator` | arm64 | Native host/runtime helpers |
| `test_host_runtime_abi` | arm64 | Host ABI regression test |
| `metalsharp_core` and `metalsharp_loader` | x86_64 | Wine/Rosetta native PE loader |
| D3D11, D3D12, DXGI, audio/input, and OpenGL shims | x86_64 | Wine/Rosetta DLL surface |
| `metalsharp`, EAC substrate, and other Wine-side native artifacts | x86_64 | Wine/Rosetta launch surface |
| Other native CTest targets | x86_64 | Tests for the Wine-side surface |

`METALSHARP_WINE_ARCH` and `METALSHARP_HOST_ARCH` are the CMake knobs for
these two lanes. The project default remains the Wine architecture so vendor
libraries and Wine-side tests resolve consistently; host targets use
`metalsharp_host_target()` explicitly. The host launcher intentionally does
not link `metalsharp_core`, which is an x86_64 PE-facing library.

The macOS architecture regression test uses `lipo -info`/`lipo -archs` to
check the configured target outputs. The native-shim, host-runtime, bundle,
and DMG validators repeat the relevant checks so a wrong-architecture binary
cannot silently enter the packaged arm64 application.
