# MetalSharp Architecture
**Updated:** 2026-10-28

How MetalSharp launches games, what it installs, and where state lives. Build instructions are in [development.md](development.md) and [building-wine.md](building-wine.md); emulator providers are in [emulators.md](emulators.md).

MetalSharp 1.0.0 comes in two builds from the same source. The backend picks its build at compile time (`make VARIANT=fex` defines `MS_BACKEND_VARIANT`), and the app follows the backend's `variant` in `GET /status`:

- **Regular** (`MetalSharp-<version>-arm64.dmg`): the x86_64 Wine 11.17 runtime under Rosetta. Most of this document describes it.
- **Arm64 FEX** (`MetalSharp-<version>-arm64-FEX.dmg`): MetalSharp Wine, a native arm64 Wine 11.20 that runs x86 code with FEX. See [Arm64 FEX Build](#arm64-fex-build).

## Launch Flow

The C backend (`~/.metalsharp/run/backend.sock`) owns every launch. Route selection is data-driven: adding a game means editing its rule set, not backend code.

### Rule sets

Each build has its own rule set, with a `[rule_set]` table naming the build, version and schema:

| Build | File | Pipelines |
|---|---|---|
| Regular (x86_64 Wine under Rosetta) | `configs/mtsp-rules.toml` | `dxmt`, `dxmt_32`, `d3d9`, `vkd3d`, `d3dmetal`, `wine_bare`, `fna_arm64`, `fna_x86` |
| Arm64 FEX (MetalSharp Wine) | `configs/mtsp-rules-fex.toml` | `dxmt`/`dxmt_32` (Mullion), `vkd3d` (VKD3D-Proton), `dxvk`, `wine_bare` (OpenGL), `fna_x86` |

The regular backend compiles its rules into `app/src-c/runtime/mtsp_catalog.c`. The FEX backend reads `mtsp-rules-fex.toml` from `~/.metalsharp/configs`, which setup stages from the app's `Resources/configs`, and applies its lanes over the compiled catalog. Rules the file doesn't name use the regular rule's lane (`ms_mtsp_fex_lane`). Don't edit the FEX file by hand: `tools/rules/generate-fex-rules.py` generates it from the regular rules with that same translation. D3DMetal and GPTK titles go to VKD3D-Proton, the D3D9 route to DXVK, and arm64 FNA titles to Mullion. Then the per-game lanes verified on MetalSharp Wine (`FEX_VERIFIED` in the generator) apply. CI runs the generator with `--check` and validates both files with `tools/ci/validate-rules-toml.py`.

```text
Play clicked
  -> app calls the backend (e.g. POST /steam/launch-game {"appid":<appid>,"launchMethod":"<pipeline>"})
  -> backend resolves the route: TOML rule -> managed/FNA eligibility -> PE imports -> directory heuristics -> fallback
  -> backend syncs and preflights the bottle, builds a LaunchRecipe
  -> regular build: backend copies route DLLs beside the game executable and sets Wine/DYLD/cache env
     Arm64 FEX build: backend writes the route to <game>/.metalsharp/launch.env and stages no DLLs
  -> Wine (or native Mono) starts the game; Wine Steam stays alive in the background for Steamworks
```

Pipeline endpoints: `GET /mtsp/pipelines`, `POST /mtsp/prepare`.

## Graphics Routes

The regular build's routes (the Arm64 FEX build's lanes are under [Arm64 FEX Build](#arm64-fex-build)):

| Route | APIs | Translation chain | DLLs staged beside the game |
|---|---|---|---|
| D3DMetal | D3D12 / 11 / 10 | DirectX -> D3DMetal -> Metal | Managed GPTK payload DLLs |
| DXMT | D3D11 / 10 (D3D12 lane) | DirectX -> DXMT -> winemetal -> Metal | `d3d11.dll`, `dxgi.dll`, `d3d10core.dll`, `winemetal.dll`; D3D12 adds `d3d12.dll` from `lib/dxmt_m12` |
| DXMT (32) | 32-bit D3D11 / 10 | DirectX -> DXMT i386 -> Metal | i386 DXMT payload |
| D3D9 | D3D9 | Wine i386 D3D9 with x87 acceleration (DXMT launch/cache family) | `lib/dxmt` |
| VKD3D | D3D12 / 11 / 10 / 9 | DirectX -> vkd3d-proton / DXVK -> MoltenVK -> Metal | `d3d12.dll`, `d3d12core.dll` (vkd3d-proton); `d3d11.dll`, `d3d10core.dll`, `d3d9.dll`, `dxgi.dll` (DXVK); `MoltenVK.dylib`, `MoltenVK_icd.json` |
| Mono/FNA | XNA / FNA / MonoGame | Managed code -> native Mono + FNA -> Metal (no Wine) | See Mono/FNA below |

Internal route and lane IDs used in code, configs, and cache paths:

| ID | Meaning | Payload | Shader cache |
|---|---|---|---|
| `m12` | D3D12 via DXMT | `lib/dxmt_m12` | `shader-cache/m12/<appid>/` |
| `m11` | D3D11 via DXMT | `lib/dxmt` | `shader-cache/m11/<appid>/` |
| `m10` | D3D10 via DXMT | `lib/dxmt` | `shader-cache/m10/<appid>/` |
| `m9` | D3D9 (auto-selected) | `lib/dxmt` | `shader-cache/m9/<appid>/` |
| `DXMT` | Auto-router selecting D3D12/11/10/9 from rules and PE evidence | | |
| `M32` | 32-bit Wine fallback for diagnostics/legacy records | | |
| `Steam`, `MacOS Steam` | Windows / native Steam client handoff for bootstrap and diagnostics | | |
| `WineBare` | Plain Wine for installers and custom apps | | |

**Route switching:** graphics cleanup removes only files that byte-match a managed payload (including D3DMetal's `nvngx-on-metalfx.dll`); modified or game-provided files stay. If a payload is incomplete, repair the runtime install.

**D3DMetal** uses `~/.metalsharp/runtime/d3dmetal-gptk4-beta2/` with the shared Steam prefix. Saving the route stages its DLLs; Play refreshes them, reconciles controller shims, and launches with `SteamAppId`/`SteamGameId`. `D3DMETAL_FRAMEWORK_PATH` names the framework *executable*; keep PE DLLs, Unix files, and framework from the same payload. Wine Steam started with **Launch Steam** inherits the D3DMetal environment; a running client is reused, not restarted.

**Shader presets:** `shader-presets/dxmt-metal/` (M11) and `shader-presets/dxmt-metal12/` (M12) hold pre-compiled caches named `<appid>.db` (SQLite `cache_18` table of metallib blobs, `air64-apple-macosx15.0.0`, MSL 3.2). On launch `deploy_preset_cache()` copies a preset if no user cache exists, otherwise merges with `INSERT OR IGNORE`. To add one, copy `~/.metalsharp/shader-cache/<engine>/<appid>/shaders_320.db` into the matching folder as `<appid>.db`.

## Wine Runtime

**Wine 11.17**, x86_64 Unix host (Rosetta) with i386 + x86_64 PE WoW64. Check with `~/.metalsharp/runtime/wine/bin/wine --version`; the version string alone doesn't identify the patched build, so see [building-wine.md](building-wine.md).

- **MSYNC:** `WINEMSYNC` follows the `msync` setting; the wineserver keeps its choice for its lifetime, so shut down managed Wine before changing it.
- **GSBASE:** one canonical `ntdll.so`. DXMT enables the TEB/TSD swap; D3DMetal and bare Wine keep legacy behavior unless `WINE_MACOS_GSBASE_SWAP=on`.
- **Retina:** on by default for the shared prefix (`RetinaMode`, 192 DPI; off restores 96). Restart Wine Steam after changing it.
- **`metalsharp-wine` wrapper** preserves the backend's route-specific `WINEDLLPATH`, `DYLD_FALLBACK_LIBRARY_PATH`, and Vulkan ICD settings.

**WineMetalGL** (OpenGL) comes from [WineMetalGL 3.7.0](https://github.com/metalsharp/WineMetalGL/releases/tag/winemetalgl-3.7.0) (`winemetalgl-3.7.0-x86_64.tar.gz`, in the runtime bundle since 0.80.0): its core, `libwinemetalgl.dylib` (x86_64), sits next to `winemac.so`, which loads it. The runtime's `winemac.so`, `win32u.so` and `opengl32` (`.so`, and `.dll` for x86_64 and i386) are still WineMetalGL 2.2.0's patched Wine modules, built from MetalSharp's Wine; the 3.x core keeps the driver interface they load it through. The runtime also carries the 3.x Wine module (`winemetalgl.so`, and `winemetalgl.dll` for x86_64 and i386), which a prefix uses only with the module's forwarding `opengl32.dll` and `opengl32=native`; MetalSharp's prefixes do not install it. It gives programs OpenGL 4.6 core and compatibility contexts on Metal; `WINEMETALGL=0` selects Apple's OpenGL. DOOM (2016) runs its OpenGL renderer on it. Executables listed in `later_gl_entry_point_apps` (`steam_actions.c`) get `LaterEntryPoints` in the Steam prefix's registry, for OpenGL loaders that require entry points of later GL versions. The old sidecar `metalsharp-opengl.dylib` is no longer in the runtime, and setup no longer requires it; nothing loaded it. Artifact hashes are in `tools/bundles/wine-runtime-hashes.tsv`.

### Launch Environment

| Variable | Purpose |
|---|---|
| `WINEPREFIX` | Prefix location |
| `WINEDLLPATH`, `WINEDLLOVERRIDES` | PE DLL lookup and native/builtin overrides |
| `DYLD_FALLBACK_LIBRARY_PATH` | Unix library lookup for Wine and DXMT |
| `ROSETTA_ADVERTISE_AVX` | Rosetta reports translated AVX/AVX2 (no emulation added) |
| `DXMT_SHADER_CACHE_PATH`, `DXMT_CONFIG_FILE` | DXMT cache and config |
| `D3DMETAL_RUNTIME_DIR`, `D3DMETAL_FRAMEWORK_PATH` | D3DMetal payload root and framework executable |
| `SteamAppId`, `SteamGameId` | Steam identity for direct launches |
| `WINEMSYNC` | MSYNC selection |

## Arm64 FEX Build

**MetalSharp Wine** ([metalsharp/MetalSharp-Wine](https://github.com/metalsharp/MetalSharp-Wine), v4.0 for MetalSharp 1.0.0) is Wine 11.20 built as a native arm64 macOS app. Its FEX fork runs x86_64 (ARM64EC) and i386 (WoW64) Windows code, so the build needs no Rosetta. The FEX DMG carries the signed and notarized `MetalSharp-Wine.app` unchanged in `Resources/`. Setup copies it with `ditto` to `~/.metalsharp/runtime/MetalSharp-Wine.app` and makes it read-only: re-signing would drop its cross-architecture entitlement. `~/.metalsharp/runtime/wine` is a facade over the app (`wine_runtime.c`). Its `bin/` scripts run the app's wine, `lib` and `share` link to the app's, and `etc/` holds MetalSharp's own `mullion.conf`, so code that runs `runtime/wine/bin/metalsharp-wine` runs MetalSharp Wine.

| Lane | APIs | Chain |
|---|---|---|
| Mullion (`dxmt`, `dxmt_32`) | D3D12 / 11 / 10 for x86_64; D3D11 / 10 for i386 | DirectX -> Mullion (a DXMT fork) -> Metal; the default lane |
| VKD3D-Proton (`vkd3d`) | D3D12 | DirectX -> VKD3D-Proton -> MoltenVK -> Metal; the regular build's D3DMetal and GPTK titles run here |
| DXVK (`dxvk`) | D3D11 / 10 / 9, x86_64 | DirectX -> DXVK -> MoltenVK -> Metal; the regular build's D3D9 route runs here |
| OpenGL (`wine_bare`) | OpenGL, Wine's Direct3D | Wine's wined3d over WineMetalGL; games with an OpenGL renderer are asked to use it |

- **Routing:** the backend writes each game's route to `<game>/.metalsharp/launch.env` (and the executable's folder). MetalSharp Wine applies it however the game starts: from Steam (`steam://run`, so cloud saves sync), a launcher, or directly. `launch-exe` and `launch-args` beside it pick the executable and its arguments. MetalSharp Wine's loader routes D3D and DXGI DLLs to the lane's copies inside the app by name, so no DLLs are staged in game folders.
- **Rules:** `configs/mtsp-rules-fex.toml` (see [Rule sets](#rule-sets)).
- **Caches:** each game's caches go in `shader-cache/fex-<lane>/<appid>/` and `pipeline-cache/fex-<lane>/<appid>/` (`MULLION_SHADER_CACHE_PATH`, `VKD3D_SHADER_CACHE_PATH`, `DXVK_STATE_CACHE_PATH`). Settings shows and clears them per lane. Metal's pipeline cache for Mullion games is shared by every Mullion game and lives in the user's cache folder (`$(getconf DARWIN_USER_CACHE_DIR)mullion/<exe>/com.apple.metal`), so Settings clears it only after asking.
- **Settings:** per-lane frame rate limits (`/framerate`, Mullion's in `mullion.conf`), the Metal HUD, and ray tracing on the VKD3D-Proton lane. There are no x87 or controller-input settings: FEX runs x87 code, and MetalSharp Wine's own XInput and DirectInput see controllers through SDL.
- **Prefixes:** setup prepares each prefix once (`wineboot -u`, VC++ 2022 x64 and x86, core fonts). Moving an x86_64 install to the FEX build backs up each prefix's `system.reg`, `user.reg`, `userdef.reg` and `drive_c/windows` to `.metalsharp-x86_64-backup`, then converts it. Moving back to the regular build restores that backup.

### Updates Between Builds

`migration.c` picks a path from the running build, the build recorded in `setup.json` (`runtime_variant`; missing means the regular build) and its version (`ms_migration_path_for`):

| From -> to | Path |
|---|---|
| Regular -> regular | The regular build's migration |
| Regular (0.80.x or later) -> FEX | Full migration that converts the Steam, GOG, Ubisoft and bottle prefixes, then the one-time welcome; also when the FEX DMG is installed by hand |
| FEX -> FEX (1.0.0 -> later) | A light update: the new MetalSharp Wine and runtime pieces, user data in place, no prefix conversion |
| FEX -> regular | The regular migration after restoring the x86_64 backups; the read-only app is removed with the old runtime |

The updater (`app/updater/update.sh`) tells the builds apart by the DMG name (`FEX` in it) and the app's `MetalSharpVariant`. It installs a newer version, or the other build at the same version. `--recover` reinstalls the installed build, including the FEX build at its own version. While a migration is pending or running, the backend's startup MetalSharp Wine refresh does nothing, and an install lock (`runtime/.metalsharp-wine.lock`) keeps the two from replacing the app at once.

## Filesystem Layout

```text
~/.metalsharp/
├── runtime/
│   ├── wine/
│   │   ├── bin/                  metalsharp-wine, wine, wineserver
│   │   ├── lib/wine/             x86_64-unix, x86_64-windows, i386-windows
│   │   ├── lib/dxmt/             DXMT baseline (x86_64-unix, x86_64-windows)
│   │   ├── lib/dxmt_m12/         isolated D3D12/DXGI/winemetal
│   │   ├── lib/moltenvk-vkmt/
│   │   └── etc/                  dxmt.conf, vulkan/icd.d/MoltenVK_icd.json
│   ├── d3dmetal-gptk4-beta2/     wine/x86_64-windows, external/D3DMetal.framework
│   ├── mono-arm64/, mono-x86/    native Mono lanes
│   ├── host/                     host runtime ABI dylib + manifest
│   └── redist/                   redistributable sources
├── prefix-steam/                 shared Wine Steam prefix (external libraries mount by drive letter)
├── prefix-ubisoft/               Ubisoft Connect prefix
├── bottles/
│   ├── steam_<appid>/bottle.json readiness record pointing at prefix-steam
│   ├── epic_<appName>/prefix     per-title Epic prefix + manifest
│   ├── gog-prefix/prefix         shared GOG prefix
│   └── <id>/prefix               Sharp Library installer/app bottles
├── compatdata/<appid>/           metalsharp-compatdata.json, logs/launch-<ts>.log, assets/
├── epic/                         legendary/ state, library.json, artwork/, processes/
├── tools/legendary/legendary-0.21.0
├── sharp-library/  games/  launcher-games/epic/location.txt
├── shader-cache/<lane>/<appid>/
├── cache/                        ubisoft-connect/artwork/, thegamesdb_config.json
└── logs/
```

## Runtime Bundles

Packaging downloads assets from the [`bundles` release](https://github.com/metalsharp/MetalSharp/releases/tag/bundles) into `app/bundles/` and verifies their hashes against the release manifest and payload hash lists in `tools/bundles/`. The regular app carries all six required archives in its signed resources. Setup and Steam repair use those bundled archives; they never download replacement archives or trust loose archives in `~/.metalsharp/`. If a packaged archive is missing, reinstall the full official app. Developers can explicitly select a bundle directory with `METALSHARP_BUNDLE_DIR`.

| Asset | Contents |
|---|---|
| `metalsharp-graphics-dll.tar.zst` | `Graphics/dll/dxmt/` (DXMT v0.80 baseline, the only source for DXMT payloads), `dxvk/`, `vkd3d-proton/`, `d3dmetal/`, `wfdxcompat/` (the WFDXCompat launcher companion, staged to `~/.metalsharp/runtime/wfdxcompat/`), and `wfdxcompat-agility/` (WFDXCompat with its D3D12 frontend for Agility SDK titles, staged to `~/.metalsharp/runtime/wfdxcompat-agility/`) |
| `metalsharp-runtime.tar.zst` | Patched Wine 11.17, host ABI, managed payloads including D3DMetal |
| `metalsharp-assets.tar.zst` | Mono, Goldberg, EAC toggle, shims |
| `metalsharp-scripts-tools.tar.zst` | Updater scripts, configs, native tools, CEF helpers |
| `metalsharp-steam.tar.zst` | Steam installer and CEF wrapper |

Both DXMT lanes record state in `metalsharp-dxmt-runtime.json` (schema `metalsharp.dxmt-runtime.v2`, baseline `0.65.0-dxmt-v0.80-baseline-v1`). Diagnose drift with hashes, not version strings:

```bash
tools/bundles/verify-bundles.sh --require mac
tools/bundles/verify-bundles.sh --release
tools/bundles/verify-developer-sdk.sh dist/developer-sdk/metalsharp-d3d12-developer-sdk.tar.zst
```

## Steam

Wine Steam owns account, downloads, and sessions; MetalSharp owns the game process, bottle, compatdata, logs, and runtime assets. macOS Steam has no Proton-style `compatibilitytools.d`, so MetalSharp launches the game itself.

- **Launch:** `POST /steam/launch` -> `steam::launch_wine_steam()`; games go through `POST /steam/launch-game` -> `prepare_steam_pipeline_env()`.
- **Steam API staging:** `steam_api64.dll`, `steam_api.dll`, `steamclient64.dll`, `steamclient.dll`, `GameOverlayRenderer64.dll`, `GameOverlayRenderer.dll` beside the executable when available.
- **CEF wrapper:** `ensure_steam_launch_ready()` redeploys `steamwebhelper.exe` (original kept as `steamwebhelper_real.exe`, marker `.ms_wrapper_deployed`) in `prefix-steam/.../Steam/bin/cef/cef.win64/` whenever Steam overwrites it. If login stops rendering, verify the wrapper hash before changing launch args.
- **Native macOS games:** installs containing a `.app` are labeled Native macOS, skip the bottle/route selector, and launch via `open`.

**Compatdata** is the per-game launch ledger; the Steam game bottle is the repair surface. `POST /steam/compatdata {"appid":620,"pipeline":"vkd3d"}` ensures the bottle, refreshes assets, and returns the record. Fields: appid, name, `bottle_id`, `prefix_path`, `steam_prefix_path`, install path, runtime profile, `launch_pipeline`, `steam_identity_mode`, `compat_tool_name`, `launch_command_template`, log dir, detected assets/components, and last launch log/pid/status.

## Launchers and Stores

The installer classifier detects known launchers before generic .NET/WebView/MSI/PE heuristics, records `known_launcher:<id>` and `launcher_name:<name>` hints, and installs them through bare Wine so launchers never inherit game routes. Child games get their own bottle and route.

| Launcher | Profile |
|---|---|
| Minecraft Launcher | Java Launcher |
| EA App / Origin, Ubisoft Connect, Epic Games Launcher, Rockstar / Social Club | WebView (Gecko, WebView2, .NET 4.8, VC runtime, core fonts) |
| GOG Galaxy | Launcher |

**CEF launchers** (`libcef.dll`, `chrome_*.pak`, `vk_swiftshader.dll`, `app.asar`): the original becomes `<name>_real.exe`, `<name>.exe` is a wrapper relaunching with `--in-process-gpu --disable-gpu`, and `metalsharp-cefchildhook.dll` covers child processes. Evidence: `POST /launcher/evidence {"family":"ea"|"ubisoft"}`.

| Store | How it works | Backend routes |
|---|---|---|
| Ubisoft Connect | Installer from `https://ubi.li/4vxt9` into `prefix-ubisoft` with D3DMetal + Steam's CEF args; a writable external volume maps to `Y:`. Games are found from the prefix registry; artwork falls back Ubisoft -> SteamGridDB -> extracted icon. Per-game routes; Mono/FNA and Steam Emu hidden. | `GET /ubisoft/status`, `/ubisoft/library`; `POST /ubisoft/launch`, `/stop`, `/launch-game`, `/save-pipeline` |
| Epic | [Legendary](https://github.com/legendary-gl/legendary) 0.21.0 (arm64, size/hash-verified) out of process. Each title needs **Initialize Bottle**; mouse mode **No Recenter** (default) or **Mouse Auto**. Optional TheGamesDB key supplies missing artwork. | `GET /sharp-library/epic/{status,games,running,thegamesdb-api-key}`; `POST` `install-tool`, `auth`, `thegamesdb-api-key`, `logout`, `sync`, `install`, `progress`, `cancel`, `initialize`, `play`, `stop`, `stop-all`, `uninstall` |
| GOG | GOGDL 1.3.1 arm64 from a fixed release, checked against an embedded SHA-256 before installation. Existing helpers migrate on login, sync, play, install, or import without replacing the prefix. Shared `bottles/gog-prefix`; Stop runs `wineserver -k` on it, so it stops every GOG game. | |
| Game Jolt | Isolated process groups; routes D3DMetal, VKD3D, DXMT, DXMT (32), D3D9. | |

Sharp Library apps run in isolated process groups tracked by app ID; **Cmd+Opt+Q** sends stop-all for every store. Runtime migration preserves the Ubisoft, Epic, and GOG prefixes and data, then runs `wineboot -u` and verifies the `dosdevices/c:` and `z:` mappings (failure is an explicit error).

## Redistributables

Bottle components: `vcrun2019`, `directx_jun2010`, `dotnet48`, `webview2`, `openal`, `xna`, `physx`, `wine-mono`, `gecko`, `corefonts`. Graphics components (`d3d9`, `d3d10`, `d3d11`, `d3d12`, `dxgi`) verify from the runtime.

Steam bottles infer needs from `_CommonRedist`, `CommonRedist`, and `installscript.vdf` (e.g. `DXSETUP.exe`, `xnafx40_redist.msi`, `oalinst.exe`). Repair sources, in order: Steamworks Shared `_CommonRedist` in `prefix-steam`, `bottles/*/installers/`, `runtime/redist/`. MSIs run via `msiexec /i`, EXEs with known silent args; each repair writes a per-bottle log.

## Mono/FNA

| Lane | Runtime | Version | Used for |
|---|---|---|---|
| Wine Mono | `runtime/wine/share/mono/wine-mono-11.3.0` | 11.3.0 | CLR/bootstrapper apps inside Sharp Library bottles |
| Native Mono ARM64 | `runtime/mono-arm64/bin/mono` | 6.14.1 | Terraria-style FNA games |
| Native Mono x86_64 | `runtime/mono-x86/bin/mono` | 6.12.0 | Celeste-style legacy FNA games (Rosetta) |

The route stages FNA/XNA assemblies, native FNA3D/FAudio/SDL/Steam libraries, FMOD stubs, `steam_appid.txt`, Steamworks shims, and dllmap shims (`libkernel32.dylib`, `libuser32.dylib`, `libCarbon.dylib`, Carbon interpose, `xaudio2_9.dylib`, `xinput1_4.dylib`). Component names `fna_arm64`, `fna_x86`, `mono-arm64`, `mono-x86` stay internal.

Shim sources live in `src/fna/shims/`:

- `csteamworks_shim.c` bridges Steamworks.NET v10 `CSteamworks.dll` calls to modern `libsteam_api.dylib` (`RestartAppIfNecessary` -> false, `Init` wraps `SteamAPI_InitFlat`). Build: `./build_csteamworks.sh`.
- `fmod_stub.c`, `fmodstudio_stub.c` are silent FMOD Studio 1.10 stubs (no arm64 build exists). Build: `./build_fmod_stubs.sh`.

## Host Runtime ABI

`include/metalsharp/HostRuntimeABI.h` is the boundary between Windows-facing shims, Wine unixlibs, and macOS services (process/path identity, logging, Steam bridge, managed runtime config, capability reporting). Every struct starts with `struct_size`; callers reject a different major version and tolerate larger structs.

```text
METALSHARP_HOST_ABI_VERSION_MAJOR / _MINOR
MetalSharpHostRuntimePaths       bottle_id, bottle_prefix, game_install_path, log_path
MetalSharpSteamBridgeConfig      appid, socket_path (<METALSHARP_HOME>/run/steam-bridge.sock)
MetalSharpManagedRuntimeConfig   Mono root/lib dirs
MetalSharpHostCapabilities
metalsharp_host_get_abi_version() / _query_capabilities() / _self_test()
```

Shims read `METALSHARP_MONO_LIB`, `METALSHARP_MONO_ROOT`, `METALSHARP_MONO_ASSEMBLY_DIR`, `METALSHARP_MONO_CONFIG_DIR` (falling back to `METALSHARP_HOME`/`HOME`). The Steam bridge listens on the owner-only Unix socket `$METALSHARP_HOME/run/steam-bridge.sock`; the shim only connects to a listener running as the same user. Inspect with `GET /runtime/host-abi`. Built as `libmetalsharp_host_runtime.dylib`, staged by `tools/package/create-host-runtime.sh`, installed to `runtime/host/`; tested by `tests/test_host_runtime_abi.cpp`.

| Area | Source | Status |
|---|---|---|
| D3D11/DXGI and D3D9 Wine unix bridges | `src/wine/*_pe.cpp`, `metalsharp_unix.mm`, `d3d9_unix.mm` | stable (the model for the ABI) |
| CoreAudio (XAudio2/DirectSound) | `src/audio/` | stable |
| GameController (XInput) | `src/input/` | stable |
| PE loader and Win32 shims | `src/loader/`, `src/win32/kernel32/` | stable |
| Steam bridge, kernel32/user32 shims | `src/fna/shims/steam_shim.c`, `kernel32_shim.c`, `user32_shim.c` | prototype |
| Mono unixlib bridge | `src/wine/mscoree_unix.c` | prototype |
| Runtime deploy glue | `app/src-c/runtime/steam_actions.c`, `setup.c` | prototype |
| Steamworks offline, Carbon, FMOD shims | `src/fna/shims/` (`METALSHARP_FNA_STEAM_PASSTHROUGH=1` for native passthrough) | game-specific |
| Anti-cheat database | `AntiCheatDB.h`, `src/runtime/DRMDetector.cpp` | diagnostic |

**Darwin sync map** (`src/runtime/host/DarwinSyncMap.cpp`): Event, Semaphore, Mutex, CriticalSection, and in-process WaitAny map to pthread/dispatch primitives and ship. WaitAll, a `ulock` futex, and NtSync (no macOS `/dev/ntsync`) are not ready.

## Vendor Trust Kit

Anti-cheat support is cooperative, never bypass or spoofing. `tools/package/create-vendor-trust-kit.sh` writes `dist/vendor-trust-kit/` with this doc, policy docs, and a `manifest.json` (commit, version, included files) so a vendor can review the runtime identity, launch model, and logs. Before external use, add real notarization evidence, launch logs, Launch Doctor anti-cheat JSON, and vendor contact context.
