# M9 Pipeline Map
**Updated:** 2026-07-08


M9 is the D3D9 engine path. It now uses the same DXMT-family launcher, cache, and Metal handoff conventions as M10, M11, and VKD3D instead of selecting the old DXVK/MoltenVK labels.

## Runtime Shape

```text
D3D9 game
  -> Wine
  -> bundled d3d9.dll
  -> MetalSharp D3D9 / Wine handoff
  -> DXMT-family launch environment
  -> Metal
```

The current DXMT source tree provides D3D10, D3D11, D3D12, DXGI, and winemetal targets. It does not ship a separate DXMT `d3d9.dll`, so M9 keeps the D3D9 DLL boundary at the bundled Wine/MetalSharp D3D9 handoff while removing DXVK/MoltenVK selection from MTSP.

## Engine Contract

| Field | Value |
|---|---|
| Pipeline | `M9` |
| Backend | `dxmt` |
| Launch args | none by default; `dx9`/`d3d9` select M9 as route aliases |
| Wine overrides | `d3d9=n,b;gameoverlayrenderer,gameoverlayrenderer64=d` |
| Shader cache subdir | `m9` |
| Preset fallback family | `m9`, then `dxmt-metal` |

M9 deploys:

- `d3d9.dll`

M9 does not deploy from `lib/dxvk`, does not set DXVK cache variables, and does not require a MoltenVK ICD in the MTSP launch path.

## Selection Rules

D3D9 PE imports resolve to `[m9]`. Legacy M9 variants such as `dxvk_metal32`, `m9_gl`, and `m32_vk` are no longer accepted as M9 aliases.
