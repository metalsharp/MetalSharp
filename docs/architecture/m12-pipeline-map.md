# M12 Pipeline Map
**Updated:** 2026-07-31

M12 is MetalSharp's 64-bit D3D12 route:

```text
D3D12 PE imports
  → PipelineId::M12
  → vkd3d-proton d3d12.dll + d3d12core.dll
  → DXVK dxgi.dll
  → Wine Vulkan PE/Unix bridge
  → ARM64 MoltenVK
  → Metal
```

## Runtime ownership

| Boundary | Canonical runtime path |
|---|---|
| D3D12 entrypoint | `runtime/wine/lib/vkd3d-proton/x86_64/d3d12.dll` |
| D3D12 core | `runtime/wine/lib/vkd3d-proton/x86_64/d3d12core.dll` |
| DXGI | `runtime/wine/lib/dxvk/x86_64/dxgi.dll` |
| Vulkan PE loader | `runtime/wine/build-ec/dlls/vulkan-1/x86_64-windows/vulkan-1.dll` |
| Wine Vulkan PE bridge | `runtime/wine/build-ec/dlls/winevulkan/x86_64-windows/winevulkan.dll` |
| Wine Vulkan ARM64 bridge | `runtime/wine/build-ec/dlls/winevulkan/winevulkan.so` |
| Active MoltenVK host library | `runtime/wine/build-ec/dlls/win32u/libMoltenVK.dylib` |
| Packaged MoltenVK source artifact | `runtime/wine/lib/moltenvk/libMoltenVK.dylib` |

M12 must not load `winemetal.dll`, `winemetal.so`, `dxgi_dxmt.dll`, or any `lib/dxmt_m12` artifact. M9/M10/M11 keep their separate DXMT surface.

## Bottle save and Play

An M12 bottle persists these runtime components:

- `m12_vkd3d_d3d12`
- `m12_vkd3d_d3d12core`
- `m12_dxvk_dxgi`
- `m12_winevulkan`
- `m12_moltenvk`

Saving an older M12 bottle rebuilds its component list and drops the retired DXMT-M12 component IDs. Saving or playing validates the complete runtime, stages the three PE route DLLs beside the selected executable and into the shared Steam prefix `system32`, then applies the route environment.

## Environment contract

M12 sets:

- `WINEDLLOVERRIDES=d3d12,d3d12core,dxgi=n,b;…`
- `WINEDLLPATH` containing the vkd3d-proton x86_64 lane and DXVK x86_64 lane
- `DYLD_LIBRARY_PATH` / `DYLD_FALLBACK_LIBRARY_PATH` containing Wine Vulkan, win32u, and ntdll ARM64 host directories
- `VKD3D_SHADER_CACHE_PATH`
- `DXVK_STATE_CACHE_PATH`
- `MS_GRAPHICS_BACKEND=vkd3d-proton`
- `WINEMSYNC=1`

It does not set DXMT configuration, winemetal, or DXMT cache variables. Graphics logs add vkd3d-proton, DXVK, and MoltenVK logging only when the user opts in.

## Diagnostics

`GET /diagnostics/m12/dry-run?appid=<appid>` and the generic pipeline dry-run report:

- every PE source path, size, and SHA-256;
- every Wine Vulkan/MoltenVK host boundary;
- the exact launch environment;
- structured missing artifacts with `ok: false`.

The D3D12 runtime doctor now consumes this route contract. The retired DXMT D3D12 SDK probe suite is not used to claim M12 readiness.

## Tested invariants

The deterministic backend suite verifies that:

- D3D12 PE detection selects M12 only for a compatible 64-bit route;
- M12 deploys exactly the vkd3d-proton/DXVK ownership set;
- M12 route switches remove stale DXMT DLLs;
- M12 prefix staging includes `d3d12.dll`, `d3d12core.dll`, and `dxgi.dll`;
- M12 has no DXMT/winemetal environment;
- M9/M10/M11 remain on their existing DXMT lanes;
- D3DMetal remains a distinct Homebrew GPTK prefix.
