# Mono Runtime Lanes
**Updated:** 2026-09-22

The **Mono/FNA** route runs XNA/FNA/MonoGame games through native Mono. The launcher picks the lane and shim set per game; the selector always shows the single Mono/FNA option.

## Lanes

| Lane | Runtime | Version | Used for |
|---|---|---|---|
| Wine Mono | Wine prefix component from `runtime/wine/share/mono/wine-mono-11.3.0` | 11.3.0 | Windows CLR/bootstrapper apps inside a bottle; installs into a prefix on demand |
| Native Mono ARM64 | `~/.metalsharp/runtime/mono-arm64/bin/mono` | 6.14.1 | Terraria-style FNA games |
| Native Mono x86_64 | `~/.metalsharp/runtime/mono-x86/bin/mono` | 6.12.0 | Celeste-style legacy FNA games under Rosetta |

## Rules

- Sharp Library installer bottles use Wine Mono for Windows CLR/bootstrapper apps.
- Steam games use native Mono lanes for known FNA/XNA targets.
- Hard cases such as Minecraft stay in the Wine bottle lane until evidence shows they need a native launcher bridge.
- Internal component names (`fna_arm64`, `fna_x86`, `mono-arm64`, `mono-x86`) are implementation details behind the Mono/FNA label.

## Route Contract

The Mono/FNA route stages:

- FNA/XNA assemblies
- native FNA3D/FAudio/SDL/Steam libraries when available
- FMOD/FMOD Studio stubs
- `steam_appid.txt` and Steamworks compatibility shims
- macOS framework-backed shims from the dllmaps: `libkernel32.dylib`, `libuser32.dylib`, `libCarbon.dylib`, the Carbon interpose shim, and bundled CoreAudio/GameController shims such as `xaudio2_9.dylib` and `xinput1_4.dylib`

Celeste-style games select x86_64 Mono under Rosetta; Terraria-style games can use the Terraria runtime patch/stub path. New Mono/FNA titles are added after per-game launch proof.
