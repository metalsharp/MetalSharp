# Host Shim Inventory
**Updated:** 2026-09-22

Inventory of the C, C++, and Objective-C host shims that feed the [Host Runtime ABI](host-runtime-abi.md).

## Classification

- `stable` — foundation for a supported ABI
- `prototype` — useful experiment; needs cleanup, tests, or relocation
- `game-specific` — per-game/runtime asset; not a general ABI
- `diagnostic` — evidence-backed status reporting

## Inventory

| Area | File | Current role | Class |
|---|---|---|---|
| Steam identity bridge | `src/fna/shims/steam_shim.c` | Exports Steam API symbols, talks to a localhost bridge on port `18733` | prototype |
| FNA Steamworks offline shim | `src/fna/shims/csteamworks_shim.c`, `SteamworksOffline.cs` | Offline CSteamworks/Steamworks.NET success stubs; optional native passthrough via `METALSHARP_FNA_STEAM_PASSTHROUGH=1` | game-specific |
| Win32 process/env/time shim | `src/fna/shims/kernel32_shim.c` | macOS-backed kernel32-style console, env, timing, file, process, thread functions | prototype |
| Window/message shim | `src/fna/shims/user32_shim.c` | Lightweight user32 stubs for windows, messages, focus, key state | prototype |
| Carbon interpose | `src/fna/shims/carbon_interpose.c` | Intercepts `dlopen` for Carbon, redirects to `METALSHARP_CARBON_SHIM` | game-specific |
| Carbon HIView shim | `src/fna/shims/carbon_hiview_shim.m` | Carbon/HIView compatibility symbols | game-specific |
| FMOD stubs | `src/fna/shims/fmod_stub.c`, `fmodstudio_stub.c` | Stub FMOD libraries for FNA games | game-specific |
| Mono/.NET unixlib bridge | `src/wine/mscoree_unix.c` | Loads Mono via `dlopen`, executes managed assemblies; configured through `METALSHARP_MONO_*` variables | prototype |
| Wine D3D11 unix bridge | `src/wine/metalsharp_unix.h`, `metalsharp_unix.mm`, `d3d11_pe.cpp`, `metalsharp_d3d11_pe.cpp`, `dxgi_pe.cpp` | PE-side D3D/DXGI shims dispatch to Objective-C++ Metal backend | stable |
| Wine D3D9 unix bridge | `src/wine/d3d9_pe.cpp`, `d3d9_unix.h`, `d3d9_unix.mm` | PE-side D3D9 shim dispatching to Metal via unixlib calls | stable |
| CoreAudio bridge | `src/audio/CoreAudioBackend.mm`, `XAudio2Engine.cpp`, `DirectSoundBackend.cpp` | Maps XAudio2/DirectSound to CoreAudio | stable |
| GameController bridge | `src/input/GameControllerBridge.mm`, `XInputEngine.cpp` | Maps XInput to Apple's GameController framework | stable |
| PE loader shim registry | `src/loader/PELoader.cpp`, `D3DShims.cpp` | Registers shim DLL exports, resolves imports for the native loader | stable |
| Win32 shim layer | `src/win32/kernel32/*.cpp`, `include/metalsharp/Win32Types.h`, `ExtraShims.h` | Native-loader Win32 shims for kernel32/ntdll/network/extra APIs | stable |
| Anti-cheat database | `include/metalsharp/AntiCheatDB.h`, `src/runtime/DRMDetector.cpp` | Evidence-backed support status strings for static detection | diagnostic |
| Runtime deploy glue | `app/src-c/runtime/steam_actions.c`, `setup.c` | Copies shims into runtime/game folders, assembles launch env, manifest-driven shim selection | prototype |

## Reference Pattern

The D3D9/D3D11 Wine PE-to-unix split is the model for the Host Runtime ABI:

```text
Windows PE side -> versioned structs -> dispatch id -> host Objective-C++ implementation -> Metal/CoreAudio/GameController/macOS APIs
```

The boundary is explicit, structs are versioned, host code tests independently, logs name each dispatch, and the ABI packages as a known runtime artifact. Per-game dylib/stub deployment outside the Mono/FNA manifest follows this pattern as it migrates: paths derive from bottle/compatdata/runtime manifests, IPC ports are manifest-configured, and every shim reports version/capabilities.

## Ownership

- The C backend owns high-level launch orchestration.
- The Host ABI owns low-level host service calls.
- Wine Steam owns account/session/download state.
- Game bottle/compatdata owns per-game runtime state.
- Per-game FMOD/FNA stubs and the Carbon interpose stay runtime assets, not ABI services.
