# MetalSharp Architecture
**Updated:** 2026-10-06

How MetalSharp launches games, what it installs, and where state lives. Build instructions are in [development.md](development.md) and [building-wine.md](building-wine.md); emulator providers are in [emulators.md](emulators.md).

## Launch Flow

The C backend (`127.0.0.1:9274`) owns every launch. Route selection is data-driven: adding a game means editing `configs/mtsp-rules.toml`, not backend code.

```text
Play clicked
  -> app calls the backend (e.g. POST /steam/launch-game {"appid":<appid>,"launchMethod":"<pipeline>"})
  -> backend resolves the route: TOML rule -> managed/FNA eligibility -> PE imports -> directory heuristics -> fallback
  -> backend syncs and preflights the bottle, builds a LaunchRecipe
  -> backend copies route DLLs beside the game executable and sets Wine/DYLD/cache env
  -> Wine (or native Mono) starts the game; Wine Steam stays alive in the background for Steamworks
```

Pipeline endpoints: `GET /mtsp/pipelines`, `POST /mtsp/prepare`.

## Graphics Routes

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

**WineMetalGL** (OpenGL) is built into the runtime from [WineMetalGL v0.1.0](https://github.com/metalsharp/WineMetalGL/releases/tag/v0.1.0) (`WineMetalGL-0.1.0.tar.zst`, SHA-256 `1796266dc1fe43bb15050851d2a3f43d53c1ae961c7596b193cc9b47a465a553`). The sidecar `metalsharp-opengl.dylib` is rebuilt as x86_64 and loaded by `winemac.so`; guest `opengl32.dll` ships for x86_64 and i386. Compatibility GL is the default; `VKMT_OPENGL_METAL_EXPERIMENTAL=1` enables the GLSL/SPIR-V/Metal path. Build with `tools/bundles/build-winemetalgl-x86.sh OUTPUT_DIR`; artifact hashes are in `tools/bundles/wine-runtime-hashes.tsv`.

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
├── cache/                        bundles/, ubisoft-connect/artwork/, thegamesdb_config.json
└── logs/
```

## Runtime Bundles

Assets come from the [`bundles` release](https://github.com/metalsharp/MetalSharp/releases/tag/bundles), into `app/bundles/` when packaging and `~/.metalsharp/cache/bundles/` for installer fallback. Hashes are tracked in `tools/bundles/asset-manifest.tsv`.

| Asset | Contents |
|---|---|
| `metalsharp-graphics-dll.tar.zst` | `Graphics/dll/dxmt/` (DXMT v0.80 baseline, the only source for DXMT payloads), `dxvk/`, `vkd3d-proton/`, `d3dmetal/`, and `wfdxcompat/` (the WFDXCompat launcher companion, staged to `~/.metalsharp/runtime/wfdxcompat/`) |
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
| GOG | Shared `bottles/gog-prefix`; Stop runs `wineserver -k` on it, so it stops every GOG game. | |
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
MetalSharpSteamBridgeConfig      appid, port (default 18733)
MetalSharpManagedRuntimeConfig   Mono root/lib dirs
MetalSharpHostCapabilities
metalsharp_host_get_abi_version() / _query_capabilities() / _self_test()
```

Shims read `METALSHARP_MONO_LIB`, `METALSHARP_MONO_ROOT`, `METALSHARP_MONO_ASSEMBLY_DIR`, `METALSHARP_MONO_CONFIG_DIR` (falling back to `METALSHARP_HOME`/`HOME`), and `METALSHARP_STEAM_BRIDGE_PORT`. Inspect with `GET /runtime/host-abi`. Built as `libmetalsharp_host_runtime.dylib`, staged by `tools/package/create-host-runtime.sh`, installed to `runtime/host/`; tested by `tests/test_host_runtime_abi.cpp`.

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
