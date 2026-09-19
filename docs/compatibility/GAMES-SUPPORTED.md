# Games Supported

Updated: 2026-09-10 (D3DMetal online-play status)

Tested and working games organized by pipeline. Only games confirmed playable are listed.

## Sharp Library Sources

| Source | Tested game | Launch path |
|---|---|---|
| GameJolt | The Joy of Creation: Reborn | DXMT |
| GOG | Fall of Porcupine: Prologue | DXMT |

## Test System

Games were tested from an external 1TB M.2 SSD (~5000 MB/s over USB-C 3.1) on an M4 MacBook Air with 16GB RAM.

## Pipelines

| Pipeline | Backend | Use |
|---|---|---|
| **D3DMetal** | Managed GPTK 4 beta 2 / Apple D3DMetal | D3D11/D3D12 through MetalSharp Wine 11.17, the shared Steam prefix, and game-local route DLLs. |
| **VKD3D** | Vulkan | D3D12/D3D11/D3D10/D3D9 -> Vulkan -> Metal |
| **DXMT** | DXMT | D3D11, D3D10 to Metal |
| **DXMT (32-bit)** | DXMT | D3D11, D3D10 32 Bit to Metal |
| **Mono/FNA** | MonoKickstart + FNA | XNA/FNA/MonoGame via native Mono runtime |

Internal routes (`dxmt` auto-detect, Wine Steam, macOS Steam, `wine_bare`) remain backend machinery and are not shown in bottle selectors.

---

## D3DMetal - D3D12, D3D11, D3D10 through Apple's D3DMetal Framework

| Game | AppID | Notes |
|---|---:|---|
| Elden Ring | 1245620 | Offline Play |
| ARMORED CORE VI FIRES OF RUBICON | 1888160 | Offline Play |
| High On Life | 1583230 | Online Play |
| Cyberpunk 2077 | 1091500 | Online Play |
| Ghostrunner | 1139900 | Online Play |
| Star Wars Jedi: Fallen Order | 1172830 | Online Play |
| Control: Ultimate Edition | 870780 | Online Play |
| BeamNG.drive | 284160 | Online Play |
| MECCHA CHAMELEON | 4704690 | Online Play |
| Sons Of The Forest | 1326470 | Online Play |
| Subnautica 2 | 1962700 | Online Play |
| Overwatch 2 | 2357570 | Online Play |
| Sekiro: Shadows Die Twice | 814380 | Online Play |
| Sonic Frontiers | 1237320 | Online Play |
| Black Myth: Wukong | 2358720 | Online Play |
| Borderlands 3 | 397540 | Online Play |
| The Witcher 3: Wild Hunt | 292030 | Online Play |
| Yu-Gi-Oh! Master Duel | 1449850 | Online Play |

---

## VKD3D - D3D12, D3D11, D3D10, D3D9 -> Vulkan -> Metal

| Game | AppID | Notes |
|---|---:|---|
| PEAK | 3527290 | Working |
| Hollow Knight: Silksong | 1030300 | |
| Schedule I | 3164500 | |
| Dark Deception | 332950 | |
| Portal2 | 620 | Steam-Emu Required |
| Mirror's Edge | 17410 | Sync-loading mitigation active. |
| Half-Life 2 | 220 | |
| Among Us | 945360 | Steam online play. |

---

## DXMT — D3D11, D3D10 to Metal

| Game | AppID | Notes |
|---|---:|---|
| Repo | 3241660 | |
| Cult of the Lamb | 1313140 | |
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
| SkyIsland | 2302640 | |
| Lethal Company | 1966720 | |
| Insurgency | 222880 | Launch with `-steam -secure` flags. |
| Graveyard Keeper | 599140 | |
| Brawlhalla | 291550 | |
| PlateUp! | 1599600 | | 
| Nine Sols | 1809540 | |
| Dave The Diver | 1868140 | |
| Besiege | 346010 | |
| AmongUs | 945360 | |
| Team Fortress 2 | 440 | |
| Amid Evil | 673130 | |
| Octopath Traveler II | 	1971650 | |
| Mind Scanners | 1389550 | | 

---

## DXMT (32-bit) — D3D11, D3D10 to Metal, 32-bit prefix route

| Game | AppID | Notes |
|---|---:|---|
| Hades | 1145360 | |
| The Binding Of Isaac: Rebirth | 250900 | | 
| Ori and the Blind Forest: Definitive Edition | 387290 | |
| Nidhogg 2 | 535520 | |

---

## Mono/FNA — XNA/FNA/MonoGame

| Game | AppID | Notes |
|---|---:|---|
| Celeste | 504230 | FNA/XNA assets, FMOD shims, Steamworks shim. x86_64 Mono. Install wizard fallback paths for `steam_api` detection. |
| Terraria | 105600 | TerrariaLauncher/patcher support, x86_64 Mono, XNA/FNA assemblies. |

---

## Notes

- Game cards can be tested through the route dropdown in each game's bottle workspace.
- Shader caches are per-appid and can be cleared from Settings.
- Wine Steam remains the background Steam client for installed Windows Steam games.
- Installed Wine Steam games create `steam_<appid>` bottle records for runtime asset/component preflight before launch.
- Env-dependent Steam routes keep Wine Steam alive as the background client, then launch the game executable directly with the selected pipeline, bottle prefix, route env, and Steam identity variables.
- D3DMetal uses the managed Wine runtime and shared Steam prefix too. Its graphics payload is separate from DXMT/VKD3D; see [Wine Architecture](../runtime/wine-architecture.md#d3dmetal).
