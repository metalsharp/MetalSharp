# DXMT and Vulkan Architecture
**Updated:** 2026-07-08


MetalSharp has two graphics translation families:

- **DXMT launch family**: M9/M10/M11/M12 to Metal
- **DXVK + MoltenVK**: Legacy fallback assets only; no longer selected by M9

## Pipeline Map

| Public route | Translation |
|---|---|
| **M12** | D3D12 -> vkd3d-proton -> Vulkan -> MoltenVK -> Metal (DXMT rollback via `m12Backend=dxmt`) |
| **M11** | D3D11 -> DXMT -> Metal |
| **M10** | D3D10 -> DXMT -> Metal |
| **M9** | D3D9 -> MetalSharp D3D9 -> DXMT launch family -> Metal |

`dxmt` is the internal auto-router that selects M12/M11/M10/M9. M32, Wine, and macOS Steam are backend fallback/diagnostic paths, not normal graphics-route buttons.

## DXMT

The DXMT launch family is used by M11, M10, and M9, and by M12 only when the
`m12Backend=dxmt` rollback is selected.

M10 is the D3D10 DXMT path. It deploys Wine's public `d3d10.dll` and `d3d10_1.dll` entrypoints for imported D3D10 APIs, then routes the core handoff through DXMT's `d3d10core.dll` and the shared D3D11/DXGI/winemetal stack.

DXMT-family DLLs:

| DLL | Used by |
|---|---|
| `d3d12.dll` | M12 |
| `d3d11.dll` | M12, M11, M10 |
| `dxgi.dll` | M12, M11, M10 |
| `d3d10core.dll` | M12, M11, M10 |
| `d3d10.dll`, `d3d10_1.dll` | M10 public Wine D3D10 entrypoints |
| `winemetal.dll` | M12, M11, M10 |
| `d3d9.dll` | M9 |
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
~/.metalsharp/shader-cache/m9/<appid>/
~/.metalsharp/shader-cache/m10/<appid>/
~/.metalsharp/shader-cache/m11/<appid>/
~/.metalsharp/shader-cache/m12/<appid>/
```

Older `dxmt-metal` and `dxmt-metal12` cache family names may still exist on disk from previous builds, but current MTSP
routes prefer the explicit M9/M10/M11/M12 cache namespaces.

## M9 D3D9

M9 does not select DXVK/MoltenVK. The pipeline deploys the D3D9 DLL from the bundled Wine runtime and uses the same DXMT launch/cache environment family as the other Metal pipelines.

Basic flow:

```text
Game
  -> d3d9.dll
  -> Wine / MetalSharp D3D9 handoff
  -> Metal
```

M9 cache path:

```text
~/.metalsharp/shader-cache/m9/<appid>/
```

## Current Game Notes

| Game | Best/current pipeline |
|---|---|
| Schedule 1 | M12 recommended |
| Subnautica | M11 |
| Subnautica: Below Zero | M12 recommended, M11 optimized |
| Rain World | M11, M9 also works |
| Undertale | M9 |
| Portal 2 | M9 |
| Nidhogg 2 | M9 |
| Ghostrunner | M11; M12 only if needed |
| DREDGE | Not a DXMT/FNA target yet; 32-bit Unity embedded Mono crash |
| Goat Simulator | M9, blocked before graphics by native .NET 4.0 CLR install |

## Notes

- DXMT is the internal direct-Metal auto-router, not a visible route selector option.
- M12 is the primary stable DXMT D3D engine method in source.
- M10 and M11 share the `dxmt-metal` preset fallback family.
- M9 no longer has a Vulkan/MoltenVK hop in MTSP selection.
- 32-bit and Wine fallback cases remain backend internals unless promoted to a public route.
