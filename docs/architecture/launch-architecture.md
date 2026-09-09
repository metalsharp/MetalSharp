# Launch Architecture
**Updated:** 2026-09-08


MetalSharp launches games through the C backend and the current MTSP pipeline resolver.

## Flow

```text
Play clicked
  -> renderer calls backend
  -> backend resolves a pipeline
  -> backend syncs/preflights the runtime bottle when one applies
  -> backend builds a LaunchRecipe
  -> backend preflights runtime assets
  -> backend prepares DLLs/env/cache beside the selected executable
  -> selected MTSP route starts the game; internal Steam/Wine/macOS handoffs are used only when the backend selects them
```

## Current Pipelines

| Public route | Backend | Launch path |
|---|---|---|
| VKD3D | Vulkan | Direct Wine launch with `dxvk/vkd3d-proton` D3D12/11/10/9/DXGI Dll's with updated MoltenVK 1.4.3 dylib/icd |
| **M12** - Hidden from UI | DXMT | Direct Wine launch with isolated `dxmt-m12` D3D12/D3D11/DXGI/winemetal DLLs |
| **M11(32)** | DXMT | Direct Wine launch with i386 `dxmt` D3D11/DXGI DLLs |
| **M11** | DXMT | Direct Wine launch with legacy `dxmt` D3D11/DXGI DLLs |
| **M10(32)** | DXMT | Direct Wine launch with i386 `dxmt` D3D10/D3D10core/DXGI DLLs |
| **M10** | DXMT | Direct Wine launch with legacy `dxmt` D3D10/D3D10core/DXGI DLLs |
| **M9** | DXMT launch family | Direct Wine launch with bundled `d3d9.dll` and DXMT-family cache/env |
| **Mono/FNA** | Native Mono | Native FNA/XNA/Mono runtime with FNA/XNA assemblies, native dylib staging, FMOD/FAudio/FNA3D shims, and Steamworks shim support |
| **D3DMetal** | Managed GPTK 4 beta 2 | Steam-aware direct launch through MetalSharp Wine 11.17, with game-local D3DMetal DLLs and `prefix-steam` |

D3DMetal uses the same managed Wine runtime as the other Wine-backed Steam routes, not a separate Homebrew Wine installation. Its launcher supplies `SteamAppId`/`SteamGameId`, the payload root in `D3DMETAL_RUNTIME_DIR`, and the framework executable in `D3DMETAL_FRAMEWORK_PATH`. M12 remains a separate DXMT route, not an alias for D3DMetal or VKD3D. See [Wine Architecture](../runtime/wine-architecture.md#d3dmetal).
