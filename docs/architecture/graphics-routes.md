# Graphics Routes
**Updated:** 2026-09-22

MetalSharp launches games through five public routes. The backend picks a route from the game's DirectX imports, and you can override it from the game card's launch mode dropdown.

## Routes

| Route | APIs | Translation chain |
|---|---|---|
| D3DMetal | D3D12 / D3D11 / D3D10 | DirectX -> D3DMetal -> Metal |
| DXMT | D3D11 / D3D10 | DirectX -> DXMT -> Metal |
| DXMT(32) | D3D11 / D3D10 (32-bit) | DirectX -> DXMT i386 -> Metal |
| Mono/FNA | XNA / FNA / MonoGame | Managed code -> native Mono + FNA -> Metal |
| VKD3D | D3D12 / D3D11 / D3D10 / D3D9 | DirectX -> DXVK / vkd3d-proton -> MoltenVK -> Metal |

## Launch Flow

```text
Play clicked
  -> backend picks the route
  -> backend prepares the bottle and runtime assets
  -> backend copies route DLLs beside the game executable
  -> backend sets Wine/DYLD/cache environment
  -> Wine (or native Mono) starts the game
```

## DXMT

DXMT translates D3D11/D3D10 directly to Metal. Deployed DLLs:

```text
d3d11.dll
dxgi.dll
d3d10core.dll
winemetal.dll      (Metal bridge)
```

Games importing `d3d12.dll` use the D3D12 lane, which adds `d3d12.dll` from the isolated `lib/dxmt_m12` payload. The 32-bit route deploys the i386 payload.

```text
Game -> DXMT PE DLLs -> winemetal -> Metal command buffers -> Apple GPU
```

## VKD3D

VKD3D routes DirectX through Vulkan:

```text
d3d12.dll, d3d12core.dll    vkd3d-proton
d3d11.dll, d3d10core.dll,
d3d9.dll, dxgi.dll          DXVK
MoltenVK.dylib,
MoltenVK_icd.json           Vulkan -> Metal
```

## D3DMetal

D3DMetal uses the managed GPTK payload at `~/.metalsharp/runtime/d3dmetal-gptk4-beta2/` with MetalSharp Wine 11.17 and the shared Steam prefix. Route DLLs are staged beside the game executable, and the launch environment sets `D3DMETAL_RUNTIME_DIR` and `D3DMETAL_FRAMEWORK_PATH`. See [Wine Architecture](../runtime/wine-architecture.md).

## Mono/FNA

Mono/FNA runs XNA/FNA/MonoGame games through MetalSharp's native Mono runtime with staged FNA/XNA assemblies, native dylibs, FMOD/FAudio/FNA3D shims, and Steamworks shim support. See [Mono Runtime Lanes](../runtime/mono-runtime-lanes.md).

## Internal Lane Names

The code and cache paths use internal lane names. They map to public routes as follows:

| Internal lane | Covers | Payload directory | Shader cache |
|---|---|---|---|
| `m12` | D3D12 via DXMT | `lib/dxmt_m12` | `shader-cache/m12/<appid>/` |
| `m11` | D3D11 via DXMT | `lib/dxmt` | `shader-cache/m11/<appid>/` |
| `m10` | D3D10 via DXMT | `lib/dxmt` | `shader-cache/m10/<appid>/` |
| `m9` | D3D9 (auto-selected) | `lib/dxmt` | `shader-cache/m9/<appid>/` |

## Route DLL Ownership

When the route changes, graphics cleanup removes files that byte-match a managed payload — including D3DMetal's `nvngx-on-metalfx.dll`. Modified or game-provided files stay in place.
