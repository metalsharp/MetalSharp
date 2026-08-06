# MetalSharp Host Shim Inventory

Created: 2026-05-19

Purpose: Phase 0 inventory of existing C, C++, and Objective-C host shims that can seed a formal MetalSharp Host Runtime ABI.

## Classification Key

- `stable`: useful foundation that can move toward a supported ABI with limited redesign
- `prototype`: useful experiment, but needs cleanup, tests, relocation, or broader semantics
- `game-specific`: useful for a known game/runtime path but should not become a general ABI unchanged
- `legacy-risk`: naming or behavior should be revisited before anti-cheat/vendor-facing work
- `obsolete`: should be removed or replaced

## Inventory

| Area | File | Current role | Classification | Phase 1 action |
| --- | --- | --- | --- | --- |
| Steam identity bridge | `src/fna/shims/steam_shim.c` | Exports Steam API symbols and talks to a localhost bridge on port `18733`. | prototype | Generalize into Host Runtime ABI service for Steam identity/session calls. Make port/config dynamic and per-bottle aware. |
| FNA Steamworks offline shim | `src/fna/shims/csteamworks_shim.c`, `src/fna/shims/SteamworksOffline.cs` | Provides offline CSteamworks/Steamworks.NET success stubs by default, with optional native `libsteam_api.dylib` passthrough via `METALSHARP_FNA_STEAM_PASSTHROUGH=1`. | game-specific | Keep for FNA/Mono/XNA games; do not treat as complete Steamworks compatibility. |
| Win32 process/env/time shim | `src/fna/shims/kernel32_shim.c` | Provides POSIX/macOS-backed `kernel32`-style functions for console, env, timing, file, process, and thread basics. | prototype | Split reusable process/env/time services into Host Runtime ABI; leave FNA-specific exports behind. |
| Window/message shim | `src/fna/shims/user32_shim.c` | Provides lightweight `user32` stubs for windows, messages, focus, key state, and basic UI calls. | prototype | Promote only explicit window/input services; document which calls are no-op compatibility stubs. |
| Carbon interpose | `src/fna/shims/carbon_interpose.c` | Intercepts `dlopen` for Carbon and redirects to `METALSHARP_CARBON_SHIM`. | game-specific | Keep as compatibility shim for legacy FNA/Mono/macOS paths. Do not make this a general injection pattern. |
| Carbon HIView shim | `src/fna/shims/carbon_hiview_shim.m` | Supplies Carbon/HIView compatibility symbols. | game-specific | Keep with FNA/legacy macOS shims; test only with known consumers. |
| FMOD stubs | `src/fna/shims/fmod_stub.c`, `src/fna/shims/fmodstudio_stub.c` | Stub FMOD libraries for FNA/game compatibility. | game-specific | Keep as per-game runtime assets, not ABI services. |
| Mono/.NET unixlib bridge | `src/wine/mscoree_unix.c` | Wine unixlib-style bridge that loads Mono via `dlopen` and executes managed assemblies. | prototype | Remove hardcoded paths, make bottle/runtime path configurable, add tests with known .NET installer/app cases. |
| Wine D3D11 unix bridge | `src/wine/metalsharp_unix.h`, `src/wine/metalsharp_unix.mm`, `src/wine/d3d11_pe.cpp`, `src/wine/metalsharp_d3d11_pe.cpp`, `src/wine/dxgi_pe.cpp` | PE-side D3D/DXGI shims dispatch to Objective-C++ Metal backend through Wine unixlib style calls. | stable | Use as the strongest model for Host Runtime ABI shape: PE-facing shim, host-side dispatch table, versioned structs. |
| Wine D3D9 unix bridge | `src/wine/d3d9_pe.cpp`, `src/wine/d3d9_unix.h`, `src/wine/d3d9_unix.mm` | PE-side D3D9 shim dispatches to Metal backend via unixlib calls. | stable | Align D3D9 and D3D11 dispatch/versioning patterns. |
| CoreAudio bridge | `src/audio/CoreAudioBackend.mm`, `src/audio/XAudio2Engine.cpp`, `src/audio/DirectSoundBackend.cpp` | Maps XAudio2/DirectSound-like behavior to CoreAudio. | stable | Expose audio capability and diagnostics through Host Runtime ABI. |
| GameController bridge | `src/input/GameControllerBridge.mm`, `src/input/XInputEngine.cpp` | Maps XInput-like behavior to Apple's GameController framework. | stable | Expose input capability and device state diagnostics through Host Runtime ABI. |
| PE loader shim registry | `src/loader/PELoader.cpp`, `src/loader/D3DShims.cpp` | Registers shim DLL exports and resolves imports for the native PE loader path. | stable | Keep as native-loader foundation; document boundary with Wine PE/unixlib path. |
| Win32 shim layer | `src/win32/kernel32/*.cpp`, `include/metalsharp/Win32Types.h`, `include/metalsharp/ExtraShims.h` | Larger native-loader Win32 shim set for kernel32/ntdll/network/extra APIs. | stable | Use selectively for ABI service mapping; avoid duplicating behavior between FNA C shims and C++ Win32 shims. |
| Anti-cheat database | `include/metalsharp/AntiCheatDB.h`, `src/runtime/DRMDetector.cpp` | Static detection tables now use evidence-backed support status strings instead of compatible/incompatible booleans. | diagnostic | Keep static statuses aligned with launch-recipe evidence and live protected-launch proof. |
| Offline EAC mode naming | `app/src-rust/src/installer.rs`, `app/src-rust/src/main.rs` | Installer/UI text now avoids bypass wording, but endpoint names still expose the legacy `eac-toggle` route. | legacy-risk | Audit behavior, keep naming explicit if legitimate compatibility flag, remove if bypass-like. |
| Controller input shims (PR #375) | `tools/bundles/update-lib-metalsharp.py` (xinput/dinput PE shims), deployed per `controllerInput` config (`off`/`x`/`d`) | XInput shims (`xinput1_1.dll`…`xinput1_4.dll`, `xinput9_1_0.dll`) and DInput shims (`dinput.dll`, `dinput8.dll`) shipped in `lib/metalsharp` and copied into the game folder + Steam prefix `system32`/`syswow64` on launch; previous set removed on switch, pre-existing game files backed up/restored. | stable | Keep the shim set per-mode; extend the manifest pattern below to versioned selection. |
| Runtime deploy glue | `app/src-rust/src/mtsp/launcher.rs`, `app/src-rust/src/setup.rs` | Copies shims into runtime/game folders and assembles launch env. The Mono/FNA launcher now has a native shim manifest for kernel32/user32/Carbon interpose plus bundled CoreAudio/GameController dylibs. | prototype | Extend the manifest pattern into versioned Host Runtime ABI asset selection and self-tests. |

## Strongest Existing Pattern

The D3D9/D3D11 Wine PE-to-unix split is the best model for Phase 1:

```text
Windows PE side -> versioned structs -> dispatch id -> host Objective-C++ implementation -> Metal/CoreAudio/GameController/macOS APIs
```

That pattern is safer than ad hoc interposition because:

- the boundary is explicit
- structs can be versioned
- host code can be tested independently
- logs can name each dispatch
- the ABI can be packaged as a known runtime artifact

## Weakest Current Pattern

The weakest remaining pattern is per-game dylib/stub deployment outside the Mono/FNA native shim manifest:

- hardcoded ports
- hardcoded user paths
- implicit `@loader_path` assumptions
- no ABI version
- no uniform self-test
- unclear difference between no-op compatibility stubs and real host-backed services

Phase 1 should fix this by creating a versioned Host Runtime ABI and a manifest-driven shim selection layer.

## Required Phase 1 Decisions

1. Which services are ABI services?
   - process/env/path/time/logging: yes
   - Steam identity/session bridge: yes
   - graphics/audio/input dispatch: yes
   - per-game FMOD/FNA stubs: no, keep as runtime assets
   - Carbon interpose: no, keep as legacy compatibility asset

2. Which runtime owns process launch?
   - Rust backend should own high-level launch orchestration.
   - Host ABI should own low-level host service calls.
   - Wine Steam should own account/session/download state.
   - Game bottle/compatdata should own per-game runtime state.

3. How are shims configured?
   - no hardcoded user paths
   - all paths derive from bottle/compatdata/runtime manifests
   - all local IPC ports or sockets are dynamic or manifest-configured
   - every shim can report version/capabilities

## Phase 0 Conclusions

- MetalSharp already has credible host shim foundations.
- The D3D PE/unixlib bridge should become the architectural reference.
- Several early shims are good experiments but must be made relocatable, tested, and versioned.
- Anti-cheat-facing names and compatibility claims need cleanup before vendor-facing work.
