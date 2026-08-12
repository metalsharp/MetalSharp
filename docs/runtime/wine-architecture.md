# Wine Architecture
**Updated:** 2026-08-11


MetalSharp ships a self-contained Wine runtime at:

```text
~/.metalsharp/runtime/wine/
```

It is used by the public Wine-backed routes VKD3D, DXMT, and DXMT(32). Internal fallback/diagnostic routes such as M32, Steam handoff, and plain Wine also use this runtime. Mono/FNA does not use the Wine runtime.

## Layout

```text
~/.metalsharp/runtime/wine/
├── bin/
│   ├── metalsharp-wine
│   ├── wine
│   ├── wineloader
│   └── wineserver
├── lib/
│   ├── wine/
│   │   ├── x86_64-unix/
│   │   ├── x86_64-windows/
│   │   └── i386-windows/
│   ├── dxmt/
│   │   ├── x86_64-unix/
│   │   └── x86_64-windows/
│   ├── dxmt-vkd3d/                    (DXMT VKD3D rollback lane)
│   ├── vkd3d-proton/                (default VKD3D D3D12 stack)
│   ├── dxvk/                        (shared DXGI; dxgi.dll for VKD3D)
│   └── moltenvk-vkmt/               (VKMT patched MoltenVK)
└── etc/
    ├── dxmt.conf
    └── vulkan/icd.d/MoltenVK_icd.json
```

Other runtime pieces live beside it:

```text
~/.metalsharp/runtime/
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
| VKD3D | Wine + vkd3d-proton D3D12 (default, D3D12 → Vulkan → MoltenVK); DXMT fallback via `vkd3dBackend=dxmt` |
| M11 | Wine + DXMT D3D11/DXGI |
| M10 | Wine + DXMT D3D10/D3D11/DXGI |
| VKD3D | Wine + vkd3d-proton/DXVK-macOS (D3D9/D3D10/D3D11/D3D12 via Vulkan) |

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

VKD3D:

```text
d3d12.dll          (vkd3d-proton forwarder)
d3d12core.dll      (vkd3d-proton implementation)
dxgi.dll           (DXVK, shared DXGI per vkd3d-proton design)
nvapi64.dll        (optional stub)
nvngx.dll          (optional stub)
```

The vkd3d-proton stack translates D3D12 to Vulkan and runs on the VKMT-patched
MoltenVK (Vulkan-on-Metal); `VK_ICD_FILENAMES` pins the runtime ICD. The
DXVK-macOS set (d3d9/d3d10core/d3d11/dxgi) covers D3D9/D3D10/D3D11 on the
same Vulkan path.

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
| `DYLD_FALLBACK_LIBRARY_PATH` | Unix library lookup for Wine, DXMT, and MoltenVK |
| `WINEDLLOVERRIDES` | Selects injected/native DLL behavior |
| `WINEMSYNC` | Wine msync toggle (config-driven, default `1`) |
| `DXMT_SHADER_CACHE_PATH` | DXMT shader cache (DXMT routes) |
| `DXMT_CONFIG_FILE` | DXMT config file (DXMT routes) |
| `VKD3D_SHADER_CACHE_PATH` / `DXVK_STATE_CACHE_PATH` | vkd3d-proton/DXVK caches (default VKD3D route) |
| `VK_ICD_FILENAMES` | Pins the runtime-bundled VKMT `MoltenVK_icd.json` (default VKD3D route) |
| `SteamAppId` / `SteamGameId` | Steam identity for direct Steam-bottle game launches |

### Isolation Contract (read before changing any launch code)

MetalSharp's Wine runtime is **hermetic**: Steam and every Wine-backed route must
resolve MetalSharp's own wine binary, prefix, DLLs, and environment **100% of the
time**, regardless of which other Wine-based launchers are installed on the host
(CrossOver, SakuraGiri, Whisky, Game Porting Toolkit, Homebrew wine…).

Rules:

1. **Wine binary**: always resolve via `platform::runtime_wine_binary()` /
   `ms_wine()` (i.e. `~/.metalsharp/runtime/wine/bin/metalsharp-wine`). Never
   fall back to `wine`/`wine64`/`wineserver` from PATH, `/usr/local/bin`
   (CrossOver installs CLI symlinks there), or `/opt/homebrew/bin` (GPTK).
   `launch::find_wine()` fails loudly instead of falling back.
2. **Environment ownership**: the backend sets `WINEPREFIX`, `WINEDEBUG`,
   `WINEDLLOVERRIDES`, and `DYLD_FALLBACK_LIBRARY_PATH` explicitly on every
   spawn. Launcher/wrapper scripts must never `unset` those (it would clobber
   bottle prefixes and Steam DLL overrides). `WINEDLLPATH` and
   `DYLD_FALLBACK_LIBRARY_PATH` in the wrapper must list MetalSharp's own
   directories **before** any inherited value.
3. **No foreign identity**: never export `CX_ROOT` (CrossOver's identity
   variable) or mimic another launcher's env vars.
4. **Process ownership**: kill/cleanup logic (`stop_wine_steam`,
   `is_force_kill_target`, `update.sh`, process-manager helper) must prove
   ownership from the process executable (`argv[0]`), not from arbitrary
   command-line arguments. A target must use an allowlisted executable in
   `~/.metalsharp/runtime/wine/bin/`, an explicitly allowlisted MetalSharp
   runtime helper, or an executable under an MS-owned prefix, bottle, or game
   root.
   A MetalSharp path or `.exe` token appearing only in arguments is untrusted
   and never establishes ownership. A bare `wineserver`/`wineloader`/`wine64`
   name match is never sufficient — foreign launchers run processes with those
   exact names.
5. **Vulkan ICD**: `VK_ICD_FILENAMES` must resolve inside the runtime
   (`$MS_ROOT/etc/vulkan/icd.d/MoltenVK_icd.json`) or be unset — never a
   hardcoded Homebrew path, which is absent on CrossOver-only machines.

### Process Manager stop behavior

The Process Manager's **Quit Game** action follows the same ownership boundary
as the rest of the Wine lifecycle. It considers only non-Steam Wine rows whose
command line references the resolved MetalSharp data root or the managed
`prefix-steam` prefix. A bare `wine`, `wineserver`, `wineboot`, or
`drive_c/` match is never enough to stop a process from CrossOver, Whisky,
GPTK, or another Wine installation.

When the shared Steam prefix is not active, the action first invokes
MetalSharp's bundled `runtime/wine/bin/wineserver -k` with `WINEPREFIX` set to
the managed prefix. If the prefix is shared with Wine Steam, or the bundled
server is unavailable, it falls back to SIGKILL only for the already-scoped
MetalSharp game PIDs. This preserves the live Wine Steam client while keeping
foreign Wine games outside the kill scope.

The GPTK lane is the working model for isolation: it owns its prefix
(`prefix-gptk`), its DYLD paths (`gptk_seed_dyld`), and its route DLLs. The
Steam/plain-Wine lanes must be just as self-contained.

## Steam Wrapper

Wine Steam uses the bundled `steamwebhelper.exe` wrapper. Steam updates may replace it, so MetalSharp redeploys it when preparing or launching Steam.
