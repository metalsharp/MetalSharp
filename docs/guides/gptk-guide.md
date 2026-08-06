# GPTK (D3DMetal) Guide
**Updated:** 2026-07-08


MetalSharp supports Apple's Game Porting Toolkit (GPTK) as the **D3DMetal** route. This route uses Apple's D3DMetal framework for D3D11/D3D12 translation instead of the MetalSharp DXMT routes (M9/M10/M11) or the vkd3d-proton M12 route.

GPTK is **not bundled** with MetalSharp. MetalSharp installs and uses the Homebrew GPTK app in place:

```text
/Applications/Game Porting Toolkit.app
```

Do not copy GPTK into `~/.metalsharp/runtime/gptk`, and do not mix GPTK DLLs/frameworks from a different GPTK release with Homebrew GPTK Wine. The D3DMetal route depends on the Homebrew app's matched Wine + D3DMetal payload.

## How It Works

The D3DMetal route is intentionally separate from the normal DXMT routes:

- **Homebrew-owned GPTK**: Uses `/Applications/Game Porting Toolkit.app/Contents/Resources/wine/bin/wine64` and `wineserver`.
- **Shared GPTK prefix**: D3DMetal games share `~/.metalsharp/prefix-gptk/`.
- **Homebrew route DLLs**: Prefix seeding copies D3DMetal route DLLs from Homebrew GPTK into `prefix-gptk/drive_c/windows/system32`.
- **Homebrew framework path**: Launches set `D3DMETAL_FRAMEWORK_PATH` to Homebrew GPTK's `D3DMetal.framework/D3DMetal` binary.
- **Direct game launch**: D3DMetal play launches the game exe through GPTK Wine directly. It does not launch Steam.

## Setup Flow

### 1. Save a D3DMetal bottle

When you save a game as a D3DMetal bottle, MetalSharp enters the explicit D3DMetal/GPTK lane and ensures:

```bash
brew trust --cask gcenx/wine/game-porting-toolkit   # when Homebrew requires explicit trust
brew install game-porting-toolkit
softwareupdate --install-rosetta --agree-to-license
```

MetalSharp then verifies the Homebrew GPTK app and exposes the next actions. Saving a D3DMetal bottle does **not** install VC++ through a Microsoft installer, and it does **not** seed the prefix yet.

### 2. Repair Redist

Click **Repair Redist** in the D3DMetal bottle UI. This does not run `vc_redist*.exe`.

Instead, MetalSharp copies VC runtime DLLs from its Wine runtime into the GPTK prefix:

- x64 DLLs → `~/.metalsharp/prefix-gptk/drive_c/windows/system32`
- x86 DLLs → `~/.metalsharp/prefix-gptk/drive_c/windows/syswow64`

It also writes VC runtime registry keys under:

```text
Software\Microsoft\VisualStudio\14.0\VC\Runtimes\x64
Software\Microsoft\VisualStudio\14.0\VC\Runtimes\x86
```

The action only marks installed after both DLL and registry checks pass.

### 3. Seed Prefix

Click **Seed Prefix** after **Repair Redist** succeeds. MetalSharp will:

1. Wineboot `~/.metalsharp/prefix-gptk` with Homebrew GPTK Wine.
2. Copy Homebrew GPTK route DLLs into prefix `system32`:
   - `d3d10.dll`
   - `d3d11.dll`
   - `d3d12.dll`
   - `dxgi.dll`
   - `nvapi64.dll`
   - `nvngx-on-metalfx.dll`
3. Quarantine app-local D3D/DXGI/NVAPI/Winemetal route shims near the selected game exe so they cannot override the prefix route DLLs.
4. Seed Steam/user/config launch material and write D3DMetal launch metadata.
5. Verify prefix, route DLL, redist, and launch metadata readiness.

### 4. Play D3DMetal

When the bottle is ready, **Play D3DMetal** launches the saved game exe directly through Homebrew GPTK Wine with the proven route shape:

```text
WINEPREFIX=~/.metalsharp/prefix-gptk
WINEARCH=win64
WINEDEBUG=-all
WINEESYNC=1
WINEDLLOVERRIDES=d3d10,d3d11,d3d12,dxgi,nvapi64,nvngx-on-metalfx=n,b;gameoverlayrenderer,gameoverlayrenderer64=d
D3DMETAL_FRAMEWORK_PATH=/Applications/Game Porting Toolkit.app/Contents/Resources/wine/lib/external/D3DMetal.framework/D3DMetal
DYLD_FALLBACK_LIBRARY_PATH=<Homebrew GPTK lib paths>
```

## When to Use D3DMetal vs M11/M12

| | D3DMetal | M11/M12 |
|---|---|---|
| **Translation** | Apple D3DMetal framework | DXMT for M11; vkd3d-proton → Vulkan → MoltenVK for M12 (DXMT rollback available) |
| **Wine** | Homebrew GPTK Wine | MetalSharp Wine |
| **Prefix** | Shared (`prefix-gptk`) | Shared Wine Steam prefix with bottle preflight |
| **Best for** | Games that need Apple's D3DMetal behavior | Most games, better compatibility tracking |

Use M11 or M12 as the default. Switch to D3DMetal if a game has specific rendering issues on the DXMT path or if developer notes say to use GPTK.

## GPTK Prefix Location

```text
~/.metalsharp/prefix-gptk/
├── drive_c/
│   ├── windows/
│   │   ├── system32/    Homebrew GPTK route DLLs + x64 VC runtime DLLs
│   │   └── syswow64/    x86 VC runtime DLLs
│   └── metalsharp/d3dmetal/<appid>/launch.json
├── dosdevices/
├── system.reg
└── user.reg
```

## Component Repair

The D3DMetal bottle UI exposes these user-facing actions:

| Action | Purpose |
|---|---|
| **Repair Redist** | Copy x64+x86 VC runtime DLLs into the GPTK prefix and write registry keys. |
| **Seed Prefix** / **Repair Seed** | Wineboot the GPTK prefix, copy Homebrew GPTK route DLLs into `system32`, quarantine local route shims, and seed launch metadata. |
| **Play D3DMetal** | Launch the game exe through Homebrew GPTK Wine. |

The underlying bottle component IDs remain compatible with existing bottle tooling (`gptk`, `rosetta`, `gptk_prefix`, `vcrun2019_x64`, `vcrun2019_x86`).

## Troubleshooting

- **Homebrew GPTK missing**: Save the D3DMetal bottle again or run `brew install game-porting-toolkit` manually.
- **Homebrew cask trust failure**: Run `brew trust --cask gcenx/wine/game-porting-toolkit`, then save the D3DMetal bottle again.
- **Rosetta missing**: Run `softwareupdate --install-rosetta --agree-to-license` or save the D3DMetal bottle again.
- **VC++ missing**: Click **Repair Redist**. MetalSharp copies bundled runtime DLLs and writes registry keys; it does not launch a Microsoft redist installer for this lane.
- **Prefix not ready**: Click **Seed Prefix** or **Repair Seed**.
- **Game does not see a D3D12 adapter**: Re-run **Seed Prefix** so Homebrew GPTK route DLLs are copied into prefix `system32` and app-local D3D/DXGI/NVAPI shims are quarantined.
- **External drive not visible**: Add a dosdevice symlink if needed, for example: `ln -sf /Volumes/YourDrive ~/.metalsharp/prefix-gptk/dosdevices/z:`
