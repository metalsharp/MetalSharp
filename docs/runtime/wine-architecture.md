# Wine Architecture
**Updated:** 2026-09-08


MetalSharp ships a self-contained Wine runtime at:

```text
~/.metalsharp/runtime/wine/
```

The current baseline is **Wine 11.17**, with an **x86_64 Unix host** and **i386 + x86_64 PE** WoW64 support. Apple Silicon executes the host through Rosetta; 32-bit Windows support does not imply a separate 32-bit Unix host or Steam prefix.

It is used by M12, M11, M10, M9, VKD3D, and D3DMetal. Internal fallback/diagnostic routes such as M32, Steam handoff, and plain Wine also use this runtime. Mono/FNA does not use the Wine runtime.

Check the installed version without launching Steam or creating a prefix:

```bash
~/.metalsharp/runtime/wine/bin/wine --version
```

A matching version is not sufficient to identify the patched runtime: preserve its loader, bootstrap, graphics-bridge and synchronization integrations. See [How to Build MetalSharp Wine](../guides/how-to-build-metalsharp-wine.md).

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

User/runtime state lives beside the runtime root:

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
| M12 | Wine + DXMT D3D12/D3D11/DXGI |
| M11 | Wine + DXMT D3D11/DXGI |
| M10 | Wine + DXMT D3D10/D3D11/DXGI |
| M9 | Wine + D3D9 Metal under the DXMT launch family |
| VKD3D | Wine + VKD3D-Proton/DXVK + MoltenVK; separate from M12 |
| D3DMetal | Wine + managed GPTK 4 beta 2 D3DMetal payload |

M32, Steam handoff, and plain Wine remain internal Wine-backed routes for diagnostics, bootstrap cases, legacy records, and installer/custom-app internals.

## DLL Deployment

The backend copies graphics DLLs into the game directory before launch.

M11/M10:

```text
d3d11.dll
dxgi.dll
d3d10core.dll
winemetal.dll
```

M10 deploys Wine's public `d3d10.dll` and `d3d10_1.dll` entrypoints for D3D10 imports, then uses DXMT's `d3d10core.dll` as the D3D10 handoff and shares the D3D11/DXGI/winemetal runtime with M11.

M12:

```text
d3d12.dll
d3d11.dll
dxgi.dll
d3d10core.dll
winemetal.dll
```

M9:

```text
d3d9.dll
```

## D3DMetal

D3DMetal uses the managed GPTK 4 beta 2 payload at `~/.metalsharp/runtime/d3dmetal-gptk4-beta2/`, not the Wine binary in a separate Homebrew GPTK application. For Steam games it uses the existing `~/.metalsharp/prefix-steam/`, not the historical `prefix-gptk` workflow.

Saving a resolved D3DMetal bottle stages the matching route DLLs beside the selected game executable. Play refreshes those files, checks readiness, reconciles the selected controller input shims, and calls the Steam-aware direct launcher with `SteamAppId` and `SteamGameId`. Goldberg/offline mode is not required merely because D3DMetal is selected. The UI exposes a single D3DMetal readiness indicator instead of the old Repair Redist / Seed Prefix sequence.

The launch environment includes:

```text
WINEPREFIX=<MetalSharp home>/prefix-steam
D3DMETAL_RUNTIME_DIR=<MetalSharp home>/runtime/d3dmetal-gptk4-beta2
D3DMETAL_FRAMEWORK_PATH=<MetalSharp home>/runtime/d3dmetal-gptk4-beta2/external/D3DMetal.framework/D3DMetal
```

`D3DMETAL_FRAMEWORK_PATH` must name the framework **executable**, not just the `.framework` directory. Keep the PE DLLs, Unix-side support, and framework from the same payload. Do not combine DLLs from another GPTK release.

When switching between D3DMetal, DXMT/M12, and VKD3D, graphics cleanup removes only files byte-matching known managed payloads, including D3DMetal's `nvngx-on-metalfx.dll`. Modified or game-provided files are not indiscriminately deleted. This graphics ownership check is separate from the existing controller-shim helper's backup/restore behavior.

### Troubleshooting

- **Payload incomplete:** repair the MetalSharp runtime installation; installing Homebrew GPTK is not the repair path for this route.
- **Executable missing or wrong:** verify the game is installed and refresh its bottle. Selection must resolve the actual game executable rather than a launcher/service; persisted D3DMetal state re-evaluates the executable rules.
- **Save occurred before discovery finished:** refresh or save the bottle after the game path resolves. Play must still pass readiness and staging checks.
- **Launch fails:** inspect the bottle/runtime report and logs before altering DLLs or prefixes. Keep Wine Steam signed in for normal Steam-backed play.
- **External library issue:** check the registered Steam library path and shared prefix drive mappings; do not delete the Steam prefix or repoint its `z:` drive as a generic graphics fix.

## Prefixes

The shared Steam prefix is:

```text
~/.metalsharp/prefix-steam/
```

Steam is installed inside that prefix. External Steam libraries may be mounted into the prefix by drive letter.

Sharp Library installer/app bottles use dedicated prefixes:

```text
~/.metalsharp/bottles/<id>/prefix/
```

Steam game bottles are different: they are launch-authoritative readiness records, but their `prefix_path` currently
points at `~/.metalsharp/prefix-steam/` so Runtime Doctor and repair actions affect the prefix Wine Steam actually uses.
Wine Steam remains the live background client that stays connected for Steam games. Env-dependent Steam game launches
run the game executable directly through the selected MTSP pipeline with this prefix, route env, cache paths, and
`SteamAppId`/`SteamGameId`; client-only Steam handoff remains internal for diagnostics/bootstrap cases.

## Important Environment

| Variable | Purpose |
|---|---|
| `WINEPREFIX` | Prefix location |
| `WINEDLLPATH` | Wine PE DLL lookup |
| `DYLD_FALLBACK_LIBRARY_PATH` | Unix library lookup for Wine and DXMT |
| `WINEDLLOVERRIDES` | Selects injected/native DLL behavior |
| `DXMT_SHADER_CACHE_PATH` | DXMT shader cache |
| `DXMT_CONFIG_FILE` | DXMT config file |
| `SteamAppId` / `SteamGameId` | Steam identity for direct Steam-bottle game launches |
| `D3DMETAL_RUNTIME_DIR` | Root of the matched D3DMetal payload |
| `D3DMETAL_FRAMEWORK_PATH` | D3DMetal framework executable |
| `WINEMSYNC` | MSYNC selection, derived from MetalSharp's `msync` setting |

Wine 11.17 includes MSYNC client/server support. The Wine server retains its synchronization selection for its lifetime, so changing settings does not establish that an already-running server switched modes. Apply changes after a normal shutdown of managed Wine processes, not by deleting prefixes or killing unrelated Wine sessions.

## Steam Wrapper

Wine Steam uses the bundled `steamwebhelper.exe` wrapper. Steam updates may replace it, so MetalSharp redeploys it when preparing or launching Steam.
