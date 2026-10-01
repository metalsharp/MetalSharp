# How to Use MetalSharp
**Updated:** 2026-09-26


## Install

1. Download the latest MetalSharp DMG from [GitHub Releases](https://github.com/aaf2tbz/metalsharp/releases).
2. Drag MetalSharp into `/Applications`. Optionally use the homebrew tap to install.
3. Open it. If macOS blocks the unsigned app, go to **System Settings → Privacy & Security** and choose **Open Anyway**.
4. Run setup from inside MetalSharp — it uses the tools bundled in the app to install the Wine runtime, MetalSharp-owned graphics/runtime assets, and redistributable source material used by bottle repair. Homebrew is not required.
5. Start Wine Steam, sign in, and download a Windows game.


## Steam Games

Click **Play** from the Library page. Use the launch mode dropdown when you want to force a route:

| Mode | Use |
|---|---|
| D3DMetal | D3D12/11/10 through the managed D3DMetal framework |
| VKD3D | D3D12/11/10/9 via Vulkan -> Metal |
| DXMT | D3D11 / D3D10 to Metal |
| DXMT (32) | D3D11 / D3D10 32-bit to Metal |
| Mono/FNA | Windows XNA/FNA games through MetalSharp's native Mono runtime |

### Goldberg Steam Emulator

The Goldberg toggle enables offline play for supported games without Wine Steam running. Toggle it on from the game card — MetalSharp saves the original Steam DLLs as `.orig` and deploys the emulator with the correct appid. Toggle off to restore the originals. Goldberg is typically only required for old Steam games. 

## Sharp Library

Sharp Library is for Windows apps, demos, launchers, installers, and non-Steam programs.

Use **Install Windows Program** to select an `.exe` or `.msi`. MetalSharp may import it directly, or create an installer bottle, classify the installer, apply a known launcher recipe when one matches, launch it with the right profile, then scan for installed app candidates. Imported executables launch from their containing folder, and their selected graphics engine is applied to the Wine launch.

Sharp Library cards let you rename an app with the pencil next to its title and change its cover image with **Change Image**. Cover images may be JPEG, PNG, or WebP; use the cover-position control to adjust the crop.

Use **Tools → Open Folder** on an app card to open that app's bottle prefix in Finder when you need to add files manually. Managed runtime components should be installed or repaired through their runtime and bottle tools, not copied into the prefix by hand.

The library also provides tabs for Epic, GOG, Game Jolt, and supported emulators. These use the same library-style layout while keeping the shared applet switch and Settings controls in the top-right.

## Logs and Settings

Use **Logs** when something fails. The page has drawer sections for live logs, crash reports, and recent log files.

Use **Settings** to manage Steam API sync, backend restart, cache cleanup, runtime maintenance, and updates. Stable in-app updates use the verified recovery installer after the download completes; it asks before closing MetalSharp and stopping Steam/Wine, then resumes the normal migration handoff. FEX updates continue through the selected DMG installer.

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
- [Graphics Routes](../architecture/graphics-routes.md)
- [Supported Games](../compatibility/GAMES-SUPPORTED.md)
- [Wine Architecture](../runtime/wine-architecture.md)
- [Docs Map](../README.md)
