# Games Supported

Updated: 2026-08-05

Tested and working games organized by pipeline. Only games confirmed playable are listed.

## Test System

Games were tested from an external 1TB M.2 SSD (~5000 MB/s over USB-C 3.1) on an M4 MacBook Air with 16GB RAM.

## Pipelines

| Pipeline | Backend | Use |
|---|---|---|
| **D3DMetal** | Homebrew GPTK / Apple D3DMetal | D3D11/D3D12 via Apple's D3DMetal framework. GPTK is installed through Homebrew and is not bundled by MetalSharp. |
| **M12** | vkd3d-proton (default) / DXMT (rollback) | D3D12 to Metal via D3D12 → Vulkan → MoltenVK |
| **M11** | DXMT | D3D11 to Metal |
| **M11 (32-bit)** | DXMT | D3D11 to Metal, 32-bit prefix route |
| **M10** | DXMT | D3D10 to Metal |
| **M9** | DXMT | D3D9 to Metal |
| **Mono/FNA** | MonoKickstart + FNA | XNA/FNA/MonoGame via native Mono runtime |

Internal routes (`dxmt` auto-detect, Wine Steam, macOS Steam, `wine_bare`) remain backend machinery and are not shown in bottle selectors.

---

## D3DMetal

Games running through Homebrew GPTK and Apple's D3DMetal pipeline. D3DMetal bottles use the explicit Save → Repair Redist → Seed Prefix → Play D3DMetal flow, with route DLLs copied from `/Applications/Game Porting Toolkit.app` into the shared GPTK prefix. Mainly for Dx12 Gaming. 

| Game | AppID | Notes |
|---|---:|---|
| Elden Ring | 1245620 | Offline play. |
| ARMORED CORE VI FIRES OF RUBICON | 1888160 | Offline play. |
| High On Life | 1583230 | Also works on M12. |
| Cyberpunk 2077 | 1091500 | Offline play. |
| Ghostrunner | 1139900 | D3DMetal route confirmed. |
| Control - Ultimate Edition | 870780 | |
| Star Wars Jedi: Fallen Order | 1172380 | |
| Shadow Of The Tomb Raider | 750920 | |

---

## M12 — D3D12 to Metal

The primary/default D3D12 route runs on the vkd3d-proton stack (D3D12 → Vulkan → VKMT MoltenVK → Metal), with the legacy DXMT D3D12 stack available via the `m12Backend` setting. Confirmed working games are listed in the D3DMetal section where noted (e.g. High On Life "Also works on M12") and via the shipped MTSP rules for tested D3D12 titles.

---

## M11 — D3D11 to Metal

| Game | AppID | Notes |
|---|---:|---|
| Repo | 3241660 | |
| Cult of the Lamb | 1313140 | |
| The Witcher 3: Wild Hunt | 292030 | |
| The Wilds | 1028590 | |
| The Long Dark | 305620 | Ultra settings verified. |
| Subnautica | 264710 | |
| Subnautica: Below Zero | 848450 | |
| Rain World | 312520 | |
| Hollow Knight | 367520 | |
| Party Animals | 1260320 | Save M11 bottle, launch direct with Steam. |
| Dave the Diver | 1868140 | |
| Totally Accurate Battle Simulator | 508440 | |
| Skul: The Hero Slayer | 1147560 | |
| Crab Game | 1782210 | |
| MECCHA CHAMELEON | 4704690 | |
| SkyIsland | 2302640 | |
| Lethal Company | 1966720 | |
| Insurgency | 222880 | Launch with `-steam -secure` flags. |
| Graveyard Keeper | 599140 | |
| Brawlhalla | 291550 | |
| Black Myth: Wukong | 2358720 | Compatibility Mode. |
| Beam.ng Drive | 1067430 | |
| Ball X Pit | 2062430 | |
| Schedule I |	3164500 | |
| Nine Sols | 1809540 | |
| Skekiro Shadows Die Twice | 814380 | |
| Sons Of The Forest | 1326470 | |
| Thronefall | 2239150 | |
| 9 Kings | 2784470 | |
| Amid Evil | 673130 | |
| Borderlands 3 | 397540 | |
| Crab Champions | 774801 | |
| Plate Up! | 1599600 | |
| Rv There Yet? | 3949040 | | 
| UltraKill | 1229490 | |
| Velheim | 892970 | |
| Untitled Goose Game | 837470 | |

---

## M11 (32-bit) — D3D11 to Metal, 32-bit prefix route

| Game | AppID | Notes |
|---|---:|---|
| Inscryption | 1092790 | Binary: `Inscryption.exe`. |
| Hades | 1145360 | Binary: `x86/Hades.exe`. |
| Balatro | 2379780 | |
| The Binding of Isaac: Rebirth | 250900 | Binary: `isaac-ng.exe`. Windows OpenGL path verified on an internal drive; requires compatibility-profile context handling. |

---

## M10 — D3D10 to Metal

| Game | AppID | Notes |
|---|---:|---|
| Mind Scanners | 1389550 | |

---

## M9 — D3D9 to Metal

| Game | AppID | Notes |
|---|---:|---|
| Mirror's Edge | 17410 | Sync-loading mitigation active. |
| Half-Life 2 | 220 | |
| Portal 2 | 620 | Steam Emu supported. |
| Among Us | 945360 | Steam online play. |
| Team Fortress 2 | 440 | Steam online play. VAC works. |
| Undertale | 391540 | |

---

## Mono/FNA — XNA/FNA/MonoGame/Unity-Mono

| Game | AppID | Notes |
|---|---:|---|
| Celeste | 504230 | FNA/XNA assets, FMOD shims, Steamworks shim. x86_64 Mono |
| Necesse | 1169040 | Classic FNA (root FNA.dll + XNA names), baseline Mono |
| Terraria | 105600 | XNA lane (root-level XNA assemblies, no `_data/Managed` — classic layout), gdiplus/faudio stubs, prebuilt launcher + offline patcher |
| Stardew Valley | 413150 | MonoGame (root-level MonoGame.Framework.dll, net6.0, no `_data/Managed`), GOG Galaxy + Steamworks deps detected; modern Mono requirement |
| DREDGE | 1562430 | Unity-Mono (Unity 2021.3.5f1, MonoBleedingEdge, x86 PE32), version-matched Unity Mono runtime deployed on save; dual SDKs (Steamworks.NET + Galaxy) |

### Discovery & routing

The mono route classifies each installed game by evidence (`mono_profile.rs`):
Unity-Mono (`UnityPlayer.dll` + `MonoBleedingEdge` + `*_Data/Managed`, Unity
version read from `globalgamemanagers`), Unity IL2CPP (`GameAssembly.dll` —
routed to Wine/DXMT, never mono), FNA, MonoGame, XNA, MonoKickstart
(`<exe>.bin.osx` / `osx/libmonosgen`), and bare .NET. Bottle save deploys the
version-matched payloads (Unity Mono runtime lane, XNA assembly set, SDL3,
Carbon per profile) and records receipts; launch runs a pre-flight readiness
check (mono arch incl. Rosetta for x86, shims, SDL3, Unity lane) and
dispatches MonoKickstart games to the kickstart launcher.

### Mono runtime versions

The app installs baseline Wine Mono with the program; the in-app Mono button
upgrades to 11.2.0. Per-game requirement (baseline vs modern) is surfaced in
the profile explainer: Unity 2021+/SDL3/MonoGame titles need the modern
runtime.

---

## Notes

- Game cards can be tested through the route dropdown in each game's bottle workspace.
- Shader caches are per-appid and can be cleared from Settings.
- Wine Steam remains the background Steam client for installed Windows Steam games.
- Installed Wine Steam games create `steam_<appid>` bottle records for runtime asset/component preflight before launch.
- Env-dependent Steam routes keep Wine Steam alive as the background client, then launch the game executable directly with the selected pipeline, bottle prefix, route env, and Steam identity variables.
- D3DMetal is the exception to normal bundled-runtime routing: it uses Homebrew GPTK, a shared `~/.metalsharp/prefix-gptk`, copied x64+x86 VC runtime DLLs, and Homebrew-matched D3DMetal route DLLs in prefix `system32`.
