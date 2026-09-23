# Wine Architecture
**Updated:** 2026-09-23

MetalSharp ships a self-contained Wine runtime at:

```text
~/.metalsharp/runtime/wine/
```

The baseline is **Wine 11.17** with an x86_64 Unix host and i386 + x86_64 PE WoW64 support. Apple Silicon executes the host through Rosetta. Check the installed version without launching Steam or creating a prefix:

```bash
~/.metalsharp/runtime/wine/bin/wine --version
```

A matching version string alone does not identify the patched runtime; see [How to Build MetalSharp Wine](../guides/how-to-build-metalsharp-wine.md) for what the build carries.

## Layout

```text
~/.metalsharp/runtime/wine/
├── bin/
│   ├── metalsharp-wine
│   ├── wine
│   └── wineserver
├── lib/
│   ├── wine/
│   │   ├── x86_64-unix/
│   │   ├── x86_64-windows/
│   │   └── i386-windows/
│   ├── dxmt/
│   │   ├── x86_64-unix/
│   │   └── x86_64-windows/
│   ├── dxmt_m12/
│   └── moltenvk-vkmt/
└── etc/
    ├── dxmt.conf
    └── vulkan/icd.d/MoltenVK_icd.json
```

Other runtime pieces live beside it:

```text
~/.metalsharp/runtime/
├── d3dmetal-gptk4-beta2/
│   ├── wine/x86_64-windows/
│   └── external/D3DMetal.framework/D3DMetal
├── redist/
└── wine/
```

User and runtime state:

```text
~/.metalsharp/
├── prefix-steam/
├── bottles/
├── sharp-library/
├── games/
├── shader-cache/
├── cache/
└── logs/
```

## Route Use

| Route | Wine use |
|---|---|
| D3DMetal | Wine + managed GPTK D3DMetal payload |
| DXMT / DXMT(32) | Wine + DXMT D3D11/D3D10/DXGI (D3D12 lane uses `lib/dxmt_m12`) |
| VKD3D | Wine + VKD3D-Proton/DXVK + MoltenVK |

Mono/FNA runs on the native Mono runtime and launches without Wine. Internal Wine-backed lanes (M32, Steam handoff, plain Wine) cover diagnostics, bootstrap, and installer cases.

## DLL Deployment

The backend copies graphics DLLs into the game directory before launch. Per-route DLL sets are listed in [Graphics Routes](../architecture/graphics-routes.md).

## D3DMetal

D3DMetal uses the managed GPTK payload at `~/.metalsharp/runtime/d3dmetal-gptk4-beta2/` with the shared Steam prefix `~/.metalsharp/prefix-steam/`.

Saving a resolved D3DMetal bottle stages the route DLLs beside the selected game executable. Play refreshes those files, checks readiness, reconciles controller input shims, and launches with `SteamAppId` and `SteamGameId`. The UI exposes a single D3DMetal readiness indicator.

The launch environment includes:

```text
WINEPREFIX=<MetalSharp home>/prefix-steam
D3DMETAL_RUNTIME_DIR=<MetalSharp home>/runtime/d3dmetal-gptk4-beta2
D3DMETAL_FRAMEWORK_PATH=<MetalSharp home>/runtime/d3dmetal-gptk4-beta2/external/D3DMetal.framework/D3DMetal
```

`D3DMETAL_FRAMEWORK_PATH` names the framework **executable**. Keep the PE DLLs, Unix support files, and framework from the same payload.

When switching routes, graphics cleanup removes only files that byte-match a managed payload; game-provided files stay untouched. If the payload is incomplete, repair the MetalSharp runtime installation.

## Prefixes

The shared Steam prefix is `~/.metalsharp/prefix-steam/`; Steam is installed inside it and external Steam libraries mount into it by drive letter.

Sharp Library installer and app bottles use dedicated prefixes at `~/.metalsharp/bottles/<id>/prefix/`. Steam game bottles are launch-authoritative readiness records whose `prefix_path` points at the shared prefix, so Runtime Doctor repairs the prefix Wine Steam actually uses.

High Resolution (Retina) is on by default for the shared prefix: each managed launch applies Wine's `RetinaMode` and sets Windows DPI to 192; disabling it restores 96 DPI. Restart Wine Steam after changing it.

## Important Environment

| Variable | Purpose |
|---|---|
| `WINEPREFIX` | Prefix location |
| `ROSETTA_ADVERTISE_AVX` | MetalSharp sets this for Wine launches on Apple Silicon so Rosetta reports translated AVX/AVX2 CPU features; it does not add AVX emulation to Wine or guarantee game compatibility |
| `WINEDLLPATH` | Wine PE DLL lookup |
| `DYLD_FALLBACK_LIBRARY_PATH` | Unix library lookup for Wine and DXMT |
| `WINEDLLOVERRIDES` | Injected/native DLL behavior |
| `DXMT_SHADER_CACHE_PATH` | DXMT shader cache |
| `DXMT_CONFIG_FILE` | DXMT config file |
| `SteamAppId` / `SteamGameId` | Steam identity for direct game launches |
| `D3DMETAL_RUNTIME_DIR` | Root of the matched D3DMetal payload |
| `D3DMETAL_FRAMEWORK_PATH` | D3DMetal framework executable |
| `WINEMSYNC` | MSYNC selection, from MetalSharp's `msync` setting |

Wine 11.17 includes MSYNC client/server support. The Wine server keeps its synchronization selection for its lifetime, so apply setting changes after a normal shutdown of managed Wine processes.

## Steam Wrapper

Wine Steam uses the bundled `steamwebhelper.exe` wrapper. Steam updates may replace it, so MetalSharp redeploys it when preparing or launching Steam.
