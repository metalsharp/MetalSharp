# Launch Architecture
**Updated:** 2026-07-08


MetalSharp launches games through the Rust backend and the current MTSP pipeline resolver.

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

The launch recipe is the backend contract for click-to-play. It records the appid, selected pipeline, game directory,
selected executable, launch arguments, environment, DLL placement, runtime asset status, anti-cheat markers, and warnings.
Manual launch methods still work; they force the pipeline before the recipe is built.

Runtime bottles add the user-facing readiness contract. Sharp Library installer/app bottles own their own prefixes under
`~/.metalsharp/bottles/<id>/prefix`. Steam game bottles use ids like `steam_620` and are launch-authoritative preflight
records over the shared Wine Steam prefix, so Steam remains the running launcher/session owner while MetalSharp checks
runtime assets, redistributables, component state, and launch health.

Steam game bottle sync scans `_CommonRedist`, `CommonRedist`, and `installscript.vdf` payloads, then infers repairable
components such as VC runtime, DirectX June 2010, .NET 4.8, WebView2, OpenAL, XNA, and PhysX. Repair actions resolve
legal local assets from Steamworks Shared or `~/.metalsharp/runtime/redist/` and write per-bottle component logs.

For env-dependent Steam routes, MetalSharp keeps Wine Steam running as the background client, then launches the game
executable directly through the selected MTSP pipeline with the bottle prefix, route env, cache paths, and
`SteamAppId`/`SteamGameId`. Internal client-only Steam handoff still exists for diagnostics and bootstrap cases, but it is not exposed as a normal bottle option.

D3DMetal is an explicit GPTK lane rather than a generic bottle repair path. Saving a D3DMetal bottle installs/trusts Homebrew GPTK and Rosetta, **Repair Redist** copies x64+x86 VC runtime DLLs plus registry keys into `~/.metalsharp/prefix-gptk`, **Seed Prefix** copies Homebrew GPTK route DLLs into prefix `system32`, and **Play D3DMetal** launches the game exe directly through Homebrew GPTK Wine.

## Current Pipelines

| Public route | Backend | Launch path |
|---|---|---|
| **M12** | vkd3d-proton (default) / DXMT (`m12Backend` setting) | Direct Wine launch; default uses isolated vkd3d-proton D3D12/DXVK dxgi/VKMT MoltenVK DLLs; DXMT rollback uses `dxmt-m12` |
| **M11** | DXMT | Direct Wine launch with legacy `dxmt` D3D11/DXGI DLLs |
| **M10** | DXMT | Direct Wine launch with legacy `dxmt` D3D10/D3D11/DXGI DLLs |
| **M9** | DXMT launch family | Direct Wine launch with bundled `d3d9.dll` and DXMT-family cache/env |
| **Mono/FNA** | Native Mono | Native FNA/XNA/Mono runtime with FNA/XNA assemblies, native dylib staging, FMOD/FAudio/FNA3D shims, and Steamworks shim support |
| **D3DMetal** | Homebrew GPTK | Direct GPTK Wine launch with Homebrew D3DMetal framework and prefix-seeded Homebrew route DLLs |

Internal route IDs such as `dxmt`, `steam`, `macos_steam`, `wine_bare`, `m32`, and `m13` remain parseable for old records, diagnostics, and backend fallback behavior. `m13` is treated as legacy GPTK/D3DMetal compatibility and should not be used as a separate public route. Internal routes are intentionally hidden from normal bottle selectors.

## Resolution

The resolver checks, in order:

1. `configs/mtsp-rules.toml`
2. Managed .NET/FNA eligibility
3. PE header analysis
4. Installed game directory markers
5. M12 fallback

Common marker behavior:

| Marker | Pipeline |
|---|---|
| Known XNA/FNA managed game | Mono/FNA |
| Unity, Unreal, Source, RE Engine, or `steam_api*.dll` markers | M11 |
| `d3dx9_43.dll` or D3D9 import | M9 |
| PE imports D3D12 | M12 for 64-bit games, M11 otherwise |
| PE imports D3D11 | M11 |
| 64-bit PE imports D3D10 | M10 |
| PE imports D3D9 | M9 |

D3D10 PE imports are checked before broad Unity, Unreal, Source, RE Engine, and Steam marker heuristics so D3D10 games stay on `[m10]`.

## Runtime Prep

Runtime prep is recipe-driven. DXMT/Wine DLL overrides are deployed next to the selected executable rather than blindly
into the game root, which keeps nested layouts such as `Binaries/Win64` and launcher-heavy games from loading the wrong
binary or missing local overrides.

M11/M10/M9 read from the legacy runtime surface:

```text
~/.metalsharp/runtime/wine/lib/dxmt
```

M12 reads from its default vkd3d-proton/DXVK/MoltenVK surface:

```text
~/.metalsharp/runtime/wine/lib/vkd3d-proton   (d3d12.dll + d3d12core.dll)
~/.metalsharp/runtime/wine/lib/dxvk           (dxgi.dll)
~/.metalsharp/runtime/wine/lib/moltenvk-vkmt  (VKMT MoltenVK)
```

With the `m12Backend=dxmt` rollback, M12 reads from the isolated DXMT surface
instead:

```text
~/.metalsharp/runtime/wine/lib/dxmt-m12
```

M11/M10 copy:

- `d3d11.dll`
- `dxgi.dll`
- `d3d10core.dll`
- `winemetal.dll`

M10 is selected by 64-bit `d3d10.dll`, `d3d10_1.dll`, or `d3d10core.dll` imports. It deploys Wine's public `d3d10.dll` and `d3d10_1.dll` entrypoints plus DXMT's `d3d10core.dll`, so public D3D10 imports and the DXMT core handoff are both owned by the x86_64 M10 runtime contract.

M12 (default backend) copies:

- `d3d12.dll`
- `d3d12core.dll`
- `dxgi.dll` (DXVK lane)
- `nvapi64.dll` / `nvngx.dll` (GPU vendor stubs, shared `dxmt_m12` lane)

M12 also adds the route's unix library directories to the fallback library path: the default backend resolves `lib/wine/x86_64-unix` and `lib/moltenvk-vkmt` (Vulkan -> MoltenVK presentation, `VK_ICD_FILENAMES` pinned to the runtime ICD); the DXMT rollback resolves `lib/dxmt-m12/x86_64-unix` so `winemetal.so` and its bundled C++ sidecars are found. vkd3d-proton ships Windows DLLs only and has no unix sidecar.

M9 copies:

- `d3d9.dll`

M9 no longer accepts the legacy `dxvk_metal32`, `m9_gl`, or `m32_vk` aliases. D3D9 imports resolve to `[m9]`, and `[m9]` stays on the DXMT-family launch path instead of selecting DXVK/MoltenVK.

D3DMetal does not use MetalSharp's bundled GPTK assets because there are none. It uses Homebrew GPTK at `/Applications/Game Porting Toolkit.app`, copies the matched route DLLs (`d3d10`, `d3d11`, `d3d12`, `dxgi`, `nvapi64`, `nvngx-on-metalfx`) into `~/.metalsharp/prefix-gptk/drive_c/windows/system32`, sets `D3DMETAL_FRAMEWORK_PATH`, and launches through Homebrew GPTK Wine.

Mono/FNA does not use Wine. Wine Steam remains the background client for Windows Steam ownership/session state, while the selected MTSP route owns the game process.

## Bottles

| Bottle type | Prefix behavior | Used for |
|---|---|---|
| Installer / Sharp Library | Dedicated bottle prefix | Windows installers, launchers, demos, imported apps |
| Steam game | Shared `~/.metalsharp/prefix-steam` | Steam game preflight, runtime assets, component repair, launch health |
| D3DMetal game | Shared `~/.metalsharp/prefix-gptk` | Homebrew GPTK route, copied VC runtime DLL/registry seed, D3DMetal direct-game launch |

Steam game bottles do not replace Steam. They prepare the runtime state the game will use and keep Wine Steam alive as
the background Steamworks client/session owner. Env-dependent pipeline launches run the game executable directly with
Steam identity env; client-only Steam handoff remains internal for diagnostics/bootstrap cases.

When a title uses the Steam launch model and Goldberg is disabled, the launcher stages real Steam API, Steam client, and overlay DLLs next to the selected executable. Titles that only need Steam identity get `-steam`; `-secure` is reserved for games that explicitly require the secure launch model.

## Process Lifecycle

- Running games are tracked by the backend.
- Stop/kill actions terminate the registered process and child processes.
- Steam process management lives in `steam.rs`.
- Launching a Steam game keeps Wine Steam alive for Steam connectivity. Env-dependent routes apply route-specific
  environment to the spawned game process rather than trying to make an already-running Steam client inherit it.
- Wine Steam readiness checks fail clearly if Steam never becomes detectable, keeping launch requests below the renderer
  backend timeout instead of silently proceeding without a Steam client.
- Shader cache paths are per appid under `~/.metalsharp/shader-cache/`.
- Wine-backed launch logs include the host ABI version, host runtime path, Wine runtime path, Steam bridge port, and
  compatdata manifest path when the launch is tied to a Steam appid.
- Launch recipes classify detected anti-cheat markers into statuses such as `blocked_pending_vendor_support`,
  `unsupported_kernel_driver`, `vendor_supported_on_proton_assets_present`, `unknown`, and `user_mode_possible`.
