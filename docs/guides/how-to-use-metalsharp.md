# How to Use MetalSharp
**Updated:** 2026-07-08


## Install

1. Download the latest MetalSharp DMG from [GitHub Releases](https://github.com/aaf2tbz/metalsharp/releases).
2. Drag MetalSharp into `/Applications`. Optionally use the homebrew tap to install.
3. Open it. If macOS blocks the unsigned app, go to **System Settings → Privacy & Security** and choose **Open Anyway**.
4. Run setup from inside MetalSharp — it will install Homebrew dependencies, the Wine runtime, MetalSharp-owned graphics/runtime assets, and redistributable source material used by bottle repair.
5. Start Wine Steam, sign in, and download a Windows game.


## Steam Games

Click **Play** from the Library page. Use the launch mode dropdown when you want to force a route:

| Mode | Use |
|---|---|
| VKD3D | D3D12/11/10/9 to Metal via MoltenVK |
| M11(32) | D3D11 32Bit to Metal |
| M11 | D3D11 to Metal |
| M10(32) | D3D10 32Bit to Metal |
| M10 | D3D10 to Metal |
| M9 | D3D9 through the DXMT launch/cache family |
| Mono/FNA | Windows XNA/FNA games through MetalSharp's native Mono runtime, staged FNA/XNA assemblies, native dylibs, FMOD/FAudio/FNA3D shims, and Steamworks shim support |
| D3DMetal | Apple Game Porting Toolkit via Homebrew, using a shared GPTK prefix and Homebrew-matched D3DMetal route DLLs |

### Goldberg Steam Emulator

The Goldberg toggle enables offline play for supported games without Wine Steam running. Toggle it on from the game card — MetalSharp saves the original Steam DLLs as `.orig` and deploys the emulator with the correct appid. Toggle off to restore the originals. Must use for D3DMetal Bottles. 

## Sharp Library

Sharp Library is for Windows apps, demos, launchers, installers, and non-Steam programs.

Use **Install Windows Program** to select an `.exe` or `.msi`. MetalSharp may import it directly, or create an installer bottle, classify the installer, apply a known launcher recipe when one matches, launch it with the right profile, then scan for installed app candidates.

Optionally you can manage / login / install / launch Epic / Gog / GameJolt Games here. As well as ps2/ps3/ps4/ps5 emulators. 

## Logs and Settings

Use **Logs** when something fails. The page has drawer sections for live logs, crash reports, and recent log files.

Use **Settings** to manage Steam API sync, backend restart, cache cleanup, and runtime maintenance.

### Controller Input Shims

The sidebar has a **Controller** selector (Off / X / D) near the theme picker:

- **Off** (default) — no input shims are deployed.
- **X** — XInput shims (`xinput1_1.dll` … `xinput1_4.dll`, `xinput9_1_0.dll`) are copied into the game folder on launch and into the Steam prefix (`system32` + `syswow64`).
- **D** — DInput shims (`dinput.dll`, `dinput8.dll`) are deployed the same way.

Switching between X and D removes the previously deployed set before deploying the new one; switching to Off removes both. Files that already existed (for example a game's own `xinput1_3.dll`) are backed up and restored when the mode is switched off.

### Uninstall

Settings includes a **Danger Zone** section at the bottom with an **Uninstall MetalSharp** button. This removes all Wine prefixes, bottles, Steam, runtime, caches, and settings, then moves the app to Trash.

## Useful Docs

- [Current MetalSharp README](../../README.md)
- [Launch Architecture](../architecture/launch-architecture.md)
- [Compatdata Architecture](../runtime/compatdata-architecture.md)
- [Launcher Runtime](../runtime/launcher-runtime.md)
- [Redistributable Runtime](../runtime/redistributable-runtime.md)
- [Darwin Sync Map](../runtime/darwin-sync-map.md)
- [Steam Compatibility Tool Surface](../runtime/steam-compatibility-tool-surface.md)
- [Vendor Trust Kit](../runtime/vendor-trust-kit.md)
- [Proof Targets](../compatibility/proof-targets.md)
- [Supported Games](../compatibility/GAMES-SUPPORTED.md)
- [Wine Architecture](../runtime/wine-architecture.md)
