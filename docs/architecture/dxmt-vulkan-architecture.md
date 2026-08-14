# DXMT and Vulkan Architecture
**Updated:** 2026-07-08


MetalSharp has two graphics translation families:

- **DXMT launch family**: DXMT/DXMT(32) to Metal
- **Vulkan family**: VKD3D (vkd3d-proton + DXVK-macOS) to Metal via MoltenVK

## Pipeline Map

| Public route | Translation |
|---|---|
| **VKD3D** | D3D9/D3D10/D3D11/D3D12 -> vkd3d-proton + DXVK-macOS -> Vulkan -> MoltenVK -> Metal |
| **DXMT** | D3D10/D3D11 -> DXMT -> Metal (x86_64) |
| **DXMT(32)** | D3D10/D3D11 -> DXMT -> Metal (32-bit / i386) |

`dxmt` is the internal auto-router that selects VKD3D/DXMT/DXMT(32). M32, Wine, and macOS Steam are backend fallback/diagnostic paths, not normal graphics-route buttons.

## DXMT

The DXMT launch family is used by DXMT and DXMT(32). It deploys Wine's public `d3d10.dll` and `d3d10_1.dll` entrypoints for imported D3D10 APIs, then routes the core handoff through DXMT's `d3d10core.dll` and the shared D3D11/DXGI/winemetal stack.

DXMT-family DLLs:

| DLL | Used by |
|---|---|
| `d3d10.dll`, `d3d10_1.dll` | DXMT public Wine D3D10 entrypoints |
| `d3d10core.dll` | DXMT, DXMT(32) |
| `d3d11.dll`, `dxgi.dll` | DXMT, DXMT(32) |
| `winemetal.dll` | DXMT, DXMT(32) |
| `winemetal.so` | Unix Metal bridge |

Basic flow:

```text
Game
  -> DXMT PE DLL
  -> winemetal.so
  -> Metal command buffers
  -> Apple GPU
```

DXMT uses per-game shader caches under:

```text
~/.metalsharp/shader-cache/dxmt/<appid>/
~/.metalsharp/shader-cache/dxmt_32/<appid>/
~/.metalsharp/shader-cache/vkd3d/<appid>/
```

Older `dxmt-metal` and `dxmt-metal12` cache family names may still exist on disk from previous builds, but current MTSP
routes prefer the explicit DXMT/DXMT(32)/VKD3D cache namespaces.

## VKD3D

VKD3D is the complete Vulkan pipeline. It deploys vkd3d-proton's `d3d12.dll` + `d3d12core.dll` plus the DXVK-macOS `d3d9.dll`, `d3d10core.dll`, `d3d11.dll`, and `dxgi.dll` into the game folder, routed via `n,b` overrides on the VKMT MoltenVK lane. D3D9 games run through DXVK-macOS on the same Vulkan → MoltenVK path.

Basic flow:

```text
Game
  -> vkd3d-proton / DXVK-macOS PE DLL
  -> Vulkan
  -> MoltenVK
  -> Metal
```

VKD3D cache path:

```text
~/.metalsharp/shader-cache/vkd3d/<appid>/
```

## Current Game Notes

| Game | Best/current pipeline |
|---|---|
| Schedule 1 | VKD3D recommended |
| Subnautica | DXMT |
| Subnautica: Below Zero | VKD3D recommended, DXMT optimized |
| Rain World | DXMT, VKD3D also works |
| Undertale | VKD3D |
| Portal 2 | VKD3D |
| Nidhogg 2 | VKD3D |
| Ghostrunner | VKD3D only if needed |
| DREDGE | Not a DXMT/FNA target yet; 32-bit Unity embedded Mono crash |
| Goat Simulator | VKD3D, blocked before graphics by native .NET 4.0 CLR install |

## Notes

- DXMT is the internal direct-Metal auto-router, not a visible route selector option.
- VKD3D is the complete Vulkan pipeline (vkd3d-proton + DXVK-macOS on MoltenVK), covering D3D9/D3D10/D3D11/D3D12.
- DXMT and DXMT(32) share the `dxmt-metal` preset fallback family.
- 32-bit and Wine fallback cases remain backend internals unless promoted to a public route.
