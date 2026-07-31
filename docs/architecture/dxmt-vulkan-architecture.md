# DXMT and Vulkan Architecture
**Updated:** 2026-07-31

MetalSharp keeps its Direct3D translation owners separate:

| Public route | Translation ownership |
|---|---|
| **M12** | vkd3d-proton `d3d12`/`d3d12core` + DXVK `dxgi` → Wine Vulkan → ARM64 MoltenVK → Metal |
| **M11 / M11(32)** | DXMT D3D11/DXGI/winemetal → Metal |
| **M10 / M10(32)** | Wine D3D10 entrypoints + DXMT D3D10core/D3D11/DXGI/winemetal → Metal |
| **M9** | MetalSharp D3D9 handoff in the DXMT launch/cache family → Metal |
| **D3DMetal** | Homebrew GPTK Wine + D3DMetal framework in its own GPTK prefix |

The internal `dxmt` value remains an automatic resolver and legacy record value. It may select M12 for a D3D12 title, but M12 itself is not a DXMT backend.

## M12 ownership

M12 deliberately pairs components at their supported boundary:

```text
Windows game
  → vkd3d-proton d3d12.dll + d3d12core.dll
  → DXVK dxgi.dll
  → Wine vulkan-1.dll / winevulkan.dll / ARM64 winevulkan.so
  → pinned ARM64 libMoltenVK.dylib
  → Metal
```

The PE DLLs are loaded from:

```text
~/.metalsharp/runtime/wine/lib/vkd3d-proton/x86_64/
~/.metalsharp/runtime/wine/lib/dxvk/x86_64/
```

The host boundary is loaded from the same complete runtime under `wine/build-ec/dlls/{vulkan-1,winevulkan,win32u}`. M12 does not use `winemetal.dll`, `winemetal.so`, `dxgi_dxmt.dll`, or `lib/dxmt_m12`.

M12 uses isolated caches under `~/.metalsharp/shader-cache/m12/<appid>/` and exports `VKD3D_SHADER_CACHE_PATH` plus `DXVK_STATE_CACHE_PATH`. Backend logs are opt-in.

## DXMT ownership

M9 through M11 retain the known DXMT route. The complete runtime owns the
Wine entrypoints under `build-ec/dlls` and the DXMT payload under:

```text
~/.metalsharp/runtime/wine/build-ec/dxmt-v0.80/
```

The 64-bit M10/M11 routes load the x86-64/ARM64EC-compatible PE payload from `aarch64-windows`. M10(32)/M11(32) load i386 PE DLLs from `i386-windows`. Both guest modes cross into the same native ARM64 `aarch64-unix/winemetal.so`; there is no i386 Mach-O bridge. These routes keep `DXMT_WINEMETAL_UNIXLIB`, inline DXMT configuration, and DXMT cache variables. `DXMT_CONFIG_FILE` is added only when an optional config file exists. M12 emits none of these DXMT variables.

## D3DMetal ownership

D3DMetal is not a fallback copy of M12. It is a separately provisioned Homebrew GPTK route with `~/.metalsharp/prefix-gptk`, Homebrew-matched route DLLs, and its own Wine/wineserver. Bottle save and Play preserve that separation.

## Route-switch invariant

Before a route deploy, MetalSharp quarantines or discards stale app-local graphics DLLs. After selection:

- M12 has only vkd3d-proton `d3d12`/`d3d12core` and DXVK `dxgi` ownership.
- M9/M10/M11 have only their DXMT/Wine ownership.
- D3DMetal has only its Homebrew GPTK ownership.

Mixing `dxgi`, `d3d12`, or Unix bridges from different route owners is treated as an invalid launch shape.
