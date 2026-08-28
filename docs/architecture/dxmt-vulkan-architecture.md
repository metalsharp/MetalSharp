# DXMT and Vulkan Architecture
**Updated:** 2026-07-28


MetalSharp has two graphics translation families:

- **DXMT launch family**: M9/M10/M11/M12 to Metal
- **DXMT 32Bit Launch Family**: M10(32)/M11(32) to Metal 
- **DXVK + MoltenVK**: VKD3D D3D12/11/10/9 Via MoltenVk -> Metal

## Pipeline Map

| Public route | Translation |
|---|---|
| **VKD3D** | D3D12/11/10/9 -> Moltenvk -> Metal |
| **M12** | D3D12 -> DXMT -> Metal |
| **M11** | D3D11 -> DXMT -> Metal |
| **M10** | D3D10 -> DXMT -> Metal |
| **M9** | D3D9 -> MetalSharp D3D9 -> DXMT launch family -> Metal |
| **D3DMetal** | Homebrew Gptk D3D12/11 -> Metal |

## DXMT

_The DXMT launch family is used by M12, M11, M10, and M9_

DXMT-family DLLs:

| DLL | Used by |
|---|---|
| `d3d12.dll` | M12 - Intentionally hidden from user facing routes |
| `d3d11.dll (i386)` | M11(32) |
| `d3d11.dll` |  M11, M10 |
| `dxgi.dll` | M12, M11, M10 |
| `d3d10.dll(i386)`, `d3d10core(i386)` | M10(32) |
| `d3d10.dll`, `d3d10_1.dll` `d3d10core` | M10 |
| `winemetal.dll` | M12, M11, M10 |
| `d3d9.dll` | M9 |
| `winemetal.so` | Unix Metal bridge |

Basic flow:

```text
Game
  -> DXMT PE DLL
  -> Winemetal.so / Winemetal.dll
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

## VKD3D 

_The Vulkan Launch Family used by `VKD3D`_

VKD3D-Family Dlls:

| DLL | Used By |
|---|---|
| `d3d12.dll`,`d3d12core.dll` | VKD3D-Proton Dll's |
| `d3d11.dll` | DXVK-MacOS Dll |
| `d3d10core.dll` | DXVK-MacOS Dll |
| `d3d9.dll` | DXVK-MacOS Dll |
| `dxgi.dll` | DXVK-MacOS Dll with d3d12 support |
| `MoltenVK.dylib`, `Moltenvk_icd.json` | Metal Renderer for VKD3D |






