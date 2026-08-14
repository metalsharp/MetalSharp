# MetalSharp Mono Runtime Lanes
**Updated:** 2026-07-08


Status: June 2026 route hardening active

MetalSharp now treats Mono/FNA as a first-class public route, not a global dependency and not a synonym for Wine Mono. The visible selector shows one **Mono/FNA** option while the launcher chooses the appropriate native Mono lane and shim set per game.

## Lanes

| Lane | Runtime | Known version | Scope | Notes |
|------|---------|---------------|-------|-------|
| Wine Mono | Wine prefix component | bundled by Wine prefix | Windows CLR/bootstrapper apps inside a bottle | Used by .NET installers and Windows apps that call `mscoree.dll` through Wine. Minecraft currently reaches this lane and crashes in the native Mono/runtime path. |
| Native Mono ARM64 | `~/.metalsharp/runtime/mono-arm64/bin/mono` | 6.14.1 | Terraria/FNA ARM64 style games | This is the dinosaur path that made Terraria work: native macOS Mono, FNA/XNA dllmaps, native SDL/FNA3D/FAudio/shims, and no Wine prefix ownership. |
| Native Mono x86_64 | `~/.metalsharp/runtime/mono-x86/bin/mono` | 6.12.0.122 | Celeste/FNA legacy lane under Rosetta | This lane exists because some older FNA/Mono dependencies were x86_64-only or behaved better with Mono 6.12 and explicit dllmaps. |

## Rules

- Sharp Library installer bottles should use Wine Mono only when the target is actually a Windows CLR/bootstrapper app.
- Steam games should request native Mono lanes only for known FNA/XNA targets or proof targets that need the old macOS Mono path.
- Native Mono ARM64/x86_64 should not be injected into every Wine bottle.
- Wine Mono failures should be logged separately from native Mono/FNA failures.
- A hard case like Minecraft should remain in the Wine bottle lane until evidence shows it needs a native launcher bridge or a different Wine Mono strategy.
- The public route label is **Mono/FNA**. Internal component names such as `fna_arm64`, `fna_x86`, `mono-arm64`, and `mono-x86` remain implementation details.

## Current Mono/FNA Route Contract

The route stages FNA/XNA assemblies, native FNA3D/FAudio/SDL/Steam libraries when available, FMOD/FMOD Studio stubs, `steam_appid.txt`, and Steamworks compatibility shims. It also deploys the macOS framework-backed MetalSharp shims expected by the dllmaps: `libkernel32.dylib`, `libuser32.dylib`, `libCarbon.dylib`, the Carbon interpose shim, and bundled CoreAudio/GameController shims such as `xaudio2_9.dylib` and `xinput1_4.dylib` when present. Celeste-style games select x86_64 Mono under Rosetta; Terraria-style games can use the Terraria runtime patch/stub path. Celeste and Terraria are now supported through this lane; future Mono/FNA titles should still be promoted only after per-game launch proof.

## Phase 11 Minecraft Finding

Minecraft is a useful hard case because it exercises installer classification, bottle readiness, Wine Mono, embedded browser/runtime needs, and app detection. Current evidence shows:

- The bottle classifies as `java_launcher` and launches from a stable installer bottle.
- Fonts and Gecko can be repaired or detected correctly.
- WebView2 is still not available as a local runtime asset.
- bare Wine and `WINEDLLOVERRIDES=mscompatdb=d` reproduce the same native Mono crash.

The next implementation path is to keep Minecraft as the proof target while adding better Wine Mono diagnostics and, if needed, a bottle-scoped alternate Mono strategy rather than changing global runtime behavior.
