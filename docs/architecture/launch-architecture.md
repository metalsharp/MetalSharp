# Launch Architecture
**Updated:** 2026-09-22

MetalSharp launches games through the C backend. The backend resolves the route, prepares the runtime, and starts the game.

## Flow

```text
Play clicked
  -> renderer calls the backend
  -> backend resolves the route
  -> backend syncs and preflights the runtime bottle
  -> backend builds a LaunchRecipe
  -> backend prepares DLLs, env, and cache paths beside the game executable
  -> the route starts the game
```

## Routes

| Route | Backend | Launch path |
|---|---|---|
| D3DMetal | Managed GPTK payload | Steam-aware direct launch through MetalSharp Wine 11.17 with game-local D3DMetal DLLs |
| DXMT | Metal | Direct Wine launch with DXMT D3D11/D3D10/DXGI DLLs |
| DXMT(32) | Metal | Direct Wine launch with i386 DXMT DLLs |
| Mono/FNA | Native Mono | Native FNA/XNA runtime with staged assemblies and shims |
| VKD3D | Vulkan -> Metal | Direct Wine launch with DXVK / vkd3d-proton DLLs and MoltenVK |

See [Graphics Routes](graphics-routes.md) for per-route DLL sets and cache paths.
