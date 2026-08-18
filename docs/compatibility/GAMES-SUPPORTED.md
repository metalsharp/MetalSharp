# Games Supported

Updated: 2026-07-08

Tested and working games organized by pipeline. Only games confirmed playable are listed.

## Test System

Games were tested from an external 1TB M.2 SSD (~5000 MB/s over USB-C 3.1) on an M4 MacBook Air with 16GB RAM.

## Pipelines

| Pipeline | Backend | Use |
|---|---|---|
| **D3DMetal** | Homebrew GPTK / Apple D3DMetal | D3D11/D3D12 via Apple's D3DMetal framework. GPTK is installed through Homebrew and is not bundled by MetalSharp. |
| **VKD3D** | Vulkan | D3D12/D3D11/D3D10/D3D9 Vulkan |
| **M11** | DXMT | D3D11 to Metal |
| **M11 (32-bit)** | DXMT | D3D11 to Metal, 32-bit prefix route |
| **M10** | DXMT | D3D10 to Metal |
| **M9** | DXMT | D3D9 to Metal |
| **Mono/FNA** | MonoKickstart + FNA | XNA/FNA/MonoGame via native Mono runtime |

Internal routes (`dxmt` auto-detect, Wine Steam, macOS Steam, `wine_bare`) remain backend machinery and are not shown in bottle selectors.

---

## D3DMetal - All D3DMetal Games Require 'Steam-Emu'

Games running through Homebrew GPTK and Apple's D3DMetal pipeline. D3DMetal bottles use the explicit Save → Repair Redist → Seed Prefix → Play D3DMetal flow, with route DLLs copied from `/Applications/Game Porting Toolkit.app` into the shared GPTK prefix.

| Game | AppID | Notes |
|---|---:|---|
| Elden Ring | 1245620 | Offline Play |
| ARMORED CORE VI FIRES OF RUBICON | 1888160 | Offline Play |
| High On Life | 1583230 | Offline Play|
| Cyberpunk 2077 | 1091500 | Offline Play |
| Ghostrunner | 1139900 | Offline Play |
| Star Wars Jedi: Fallen Order | 1172830 | Offline Play | 
| Control: Ultimate Edition | 870780 | Offline Play |

---

## VKD3D

| Game | AppID | Notes |
|---|---:|---|
| PEAK | 3527290 | Graphical Issues |
| Hollow Knight: Silksong | 1030300 | |
| Schedule I | 3164500 | |
| Dark Deception | 332950 | |
| BeamNG Drive | 284160 | Launch Through Steam |
| Portal2 | 620 | Steam-Emu Required |

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
| Black Myth: Wukong | 2358720 | Launch with Steam, Compatability Mode |
| Overwatch 2 | 2357570 | Shaders Take A While To Compile |
| Sons Of The Forest | 463209 | |
| PlateUp! | 1599600 | | 
| Nine Sols | 1809540 | |
| Dave The Diver | 1868140 | |
| Besiege | 346010 | |
| AmongUs | 945360 | |
| Amid Evil | 673130 | |

---

## M11 (32-bit) — D3D11 to Metal, 32-bit prefix route

| Game | AppID | Notes |
|---|---:|---|
| Hades | 1145360 | |
| The Binding Of Isaac: Rebirth | 250900 | | 
| Balatro | 856021 | |

---

## M10 — D3D10 to Metal

| Game | AppID | Notes |
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
| Nidhogg 2 | 535520 | |
| Fallout: New Vegas | 22380 | Direct Steam Launch. |

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
- D3DMetal is the exception to normal bundled-runtime routing: it uses Homebrew GPTK, a shared `~/.metalsharp/prefix-gptk`, copied x64+x86 VC runtime DLLs, and Homebrew-matched D3DMetal route DLLs in prefix `system32`.
