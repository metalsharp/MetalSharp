# How to Use MetalSharp
**Updated:** 2026-07-08


## Install

1. Download the latest MetalSharp DMG from [GitHub Releases](https://github.com/aaf2tbz/metalsharp/releases).
2. Drag MetalSharp into `/Applications`.
3. Open it. If macOS blocks the unsigned app, go to **System Settings → Privacy & Security** and choose **Open Anyway**.
4. Run setup from inside MetalSharp — it will install Homebrew dependencies, the Wine runtime, MetalSharp-owned graphics/runtime assets, and redistributable source material used by bottle repair.
5. Start Wine Steam, sign in, and download a Windows game.

GPTK/D3DMetal is not installed during generic setup. MetalSharp installs and uses Homebrew GPTK only when you save a game as a D3DMetal bottle.

## Steam Games

After a game is installed, MetalSharp detects the Wine Steam library and creates a Steam game bottle such as `steam_620`.

The bottle is the launch-authoritative runtime record. It checks the selected profile, runtime assets, redistributables, DLL expectations, and logs before launch.

For routes such as M12, M11, M10, M9, and Mono/FNA, MetalSharp keeps Wine Steam alive in the background when Steamworks ownership/session state is needed, then launches the game executable through the selected bottle-aware MTSP pipeline. The game process receives the prepared prefix or native Mono/FNA environment, cache paths, and Steam identity variables (`SteamAppId` and `SteamGameId`) so Steamworks can bind back to the running Wine Steam client where applicable.

Internal Steam, Wine, macOS Steam, M32, and raw DXMT routes still exist for diagnostics, compatibility records, and backend fallback behavior, but they are not normal route selector choices. If Wine Steam is not detectable after startup, MetalSharp fails the launch clearly instead of hanging behind the renderer timeout.

Click **Play** from the Library page. Use the launch mode dropdown when you want to force a route:

| Mode | Use |
|---|---|
| M12 | D3D12 to Metal |
| M11 | D3D11 to Metal |
| M10 | D3D10 to Metal |
| M9 | D3D9 through the DXMT launch/cache family |
| Mono/FNA | Windows XNA/FNA games through MetalSharp's native Mono runtime, staged FNA/XNA assemblies, native dylibs, FMOD/FAudio/FNA3D shims, and Steamworks shim support |
| D3DMetal | Apple Game Porting Toolkit via Homebrew, using a shared GPTK prefix and Homebrew-matched D3DMetal route DLLs |

Use **Runtime Doctor** on a game card when a game needs VC runtime, DirectX, .NET, WebView2, fonts, or other launch assets.

### D3DMetal / GPTK Bottles

D3DMetal is an explicit bottle lane for games that should run through Apple Game Porting Toolkit instead of MetalSharp's DXMT route.

1. Save the game as a **D3DMetal** bottle. MetalSharp installs/trusts Homebrew GPTK (`brew trust --cask gcenx/wine/game-porting-toolkit` as needed, then `brew install game-porting-toolkit`) and ensures Rosetta 2 is present.
2. Click **Repair Redist**. This copies MetalSharp-bundled x64+x86 VC runtime DLLs into `~/.metalsharp/prefix-gptk/drive_c/windows/system32` and `syswow64`, then writes VC runtime registry keys.
3. Click **Seed Prefix**. This wineboots `~/.metalsharp/prefix-gptk`, copies Homebrew GPTK route DLLs from `/Applications/Game Porting Toolkit.app` into prefix `system32`, quarantines app-local D3D/DXGI/NVAPI shims, and writes D3DMetal launch metadata.
4. Click **Play D3DMetal**. MetalSharp launches the game executable directly through Homebrew GPTK Wine; it does not launch Steam for this route.

### Goldberg Steam Emulator

The Goldberg toggle enables offline play for supported games without Wine Steam running. Toggle it on from the game card — MetalSharp saves the original Steam DLLs as `.orig` and deploys the emulator with the correct appid. Toggle off to restore the originals.

Supported games include Portal 2 (appid 620) and others that work without Steam DRM validation.

## Sharp Library

Sharp Library is for Windows apps, demos, launchers, installers, and non-Steam programs.

Use **Install Windows Program** to select an `.exe` or `.msi`. MetalSharp may import it directly, or create an installer bottle, classify the installer, apply a known launcher recipe when one matches, launch it with the right profile, then scan for installed app candidates.

Apps imported from a bottle keep their `bottle_id`, launch through that bottle, and write per-bottle logs.

MoonScraper Chart Editor's Inno Setup bootstrapper is handled without its Windows installer UI because that bootstrapper crashes in macOS Wine's WoW64 runtime. MetalSharp uses the native `innoextract` tool (installing it through Homebrew when needed), extracts the portable application into its dedicated bottle, and adds the detected editor directly to Sharp Library.

## Logs and Settings

Use **Logs** when something fails. The page has drawer sections for live logs, crash reports, and recent log files.

Use **Settings** to manage Steam API sync, backend restart, cache cleanup, and runtime maintenance.

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
