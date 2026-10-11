# Supported Games
**Updated:** 2026-10-10

Games confirmed playable by users or the developer, grouped by launch route. Many more likely work. Tested on an M4 MacBook Air (16 GB) from an external USB-C SSD. Routes are described in the [README](../README.md#launching-games-and-graphics-routes).

## Sharp Library Sources

| Source | Tested game | Route |
|---|---|---|
| GameJolt | The Joy of Creation: Reborn | DXMT |
| GOG | Fall of Porcupine: Prologue | DXMT |

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
| The Witcher 3: Wild Hunt | 292030 | Online Play (Remastered not running yet, must choose classic option in steam options before downloading and playing) |
| Yu-Gi-Oh! Master Duel | 1449850 | Online Play |
| Palworld | 1623730 | Online Play|
| Hogwarts Legacy | 990080 | Online Play |
| HELLDIVERS 2 | 553850 | Online Play, Medium Settings |
| Assassin's Creed Odyssey | 812140 | |
| Eve Online | 8500 | Launches through steam with Specific Flags that enable the launcher to load |
| Far Cry 5 | Ubisoft | |
| Far Cry 6 | Ubisoft | |
| TrackMania | Ubisoft | |
| Marvel Rivals | 2767030 | Launches through Steam with -Windowed to avoid a mouse bug |
| Baldur's Gate 3 | 1086940 | |
| Watch Dogs Legions | Ubisoft | |
| Red Dead Redemption 2 | 1174180 | Also runs on VKD3D (Vulkan) |
| Grand Theft Auto V Enhanced | 3240220 | Story mode. Also runs on VKD3D |

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
| Dwarf Fortress | 975370 | |
| Caves Of Qud | 333640 | |
| Skyrim: Special Edition | 489830 | |

---

## DXMT — D3D11, D3D10 to Metal

| Game | AppID | Notes |
|---|---:|---|
| Dark Souls 3 | 374320 | |
| Repo | 3241660 | |
| Cult of the Lamb | 1313140 | |
| The Wilds | 1028590 | |
| The Long Dark | 305620 | Ultra settings verified. |
| Subnautica | 264710 | |
| Subnautica: Below Zero | 848450 | |
| Rain World | 312520 | |
| Hollow Knight | 367520 | |
| Party Animals | 1260320 | Save bottle, launch direct with Steam. |
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
| Besiege | 346010 | |
| AmongUs | 945360 | |
| Team Fortress 2 | 440 | |
| Amid Evil | 673130 | |
| Octopath Traveler II | 1971650 | |
| Mind Scanners | 1389550 | |
| Dredge | 1562430 | |
| Blasphemous 2 | 2114740 | |
| Dead Cells | 588650 | Runs under WineMetalGL |

---

## DXMT (32-bit) — D3D11, D3D10 to Metal, 32-bit prefix route

| Game | AppID | Notes |
|---|---:|---|
| Hades | 1145360 | |
| The Binding Of Isaac: Rebirth | 250900 | |
| Ori and the Blind Forest: Definitive Edition | 387290 | |
| Nidhogg 2 | 535520 | |
| Balatro | 2379780 | |

---

## D3D9 - Wine i386 D3D9 W/ x87 Acceleration

| Game | AppID | Notes |
|---|---|---|
| Portal 2 | 620 | Steam-Emu Required |
| Half Life 2 | 220 | |
| Undertale | 391540 | |

---

## Mono/FNA — XNA/FNA/MonoGame

| Game | AppID | Notes |
|---|---:|---|
| Celeste | 504230 | FNA/XNA assets, FMOD shims, Steamworks shim. x86_64 Mono. Install wizard fallback paths for `steam_api` detection. |
| Terraria | 105600 | TerrariaLauncher/patcher support, x86_64 Mono, XNA/FNA assemblies. |
