# Supported Games
**Updated:** 2026-10-28

Games confirmed playable by users or the developer, grouped by launch route. Many more likely work. Tested on an M4 MacBook Air (16 GB) from an external USB-C SSD. Routes are described in the [README](../README.md#graphics-routes).

The tables are the x86_64 build's routes. The Arm64 FEX build (early access) runs the same games on its own lanes. D3DMetal titles move to VKD3D-Proton, DXMT titles to Mullion, and D3D9 titles to DXVK, as its rule set `configs/mtsp-rules-fex.toml` sets out.

## Arm64 FEX Build — verified on MetalSharp Wine

- Dragon's Dogma 2 (2054970): Mullion
- Red Dead Redemption 2 (1174180): Mullion (runs best there)
- God of War Ragnarök (2322010): Mullion and VKD3D-Proton
- PEAK (3527290): VKD3D-Proton and Mullion

## Sharp Library Sources (Working Launchers and Games)

| Source | Tested game | Route |
|---|---|---|

| GOG | Fall of Porcupine: Prologue | Mullion |
| GOG | SpaceEngine | OpenGL |
| Ubisoft Connect | Assassin's Creed; Black Flag Resented | D3DMetal / Mullion |
| Ubisoft Connect | Trackmania | D3DMetal / Mullion |
| Ubisoft Connect | Far-Cry 5 | D3DMetal / Mullion |
| Ubisoft Connect | Far-Cry 6 | D3DMetal / Mullion |
| Ubisoft Connect | Watch Dogs 2; Legions | D3DMetal / Mullion |
| Epic Games | Pixel Gun 3D | D3DMetal / Mullion |
| Rockstar | GTA V | D3DMetal / Mullion |
| Rockstar | Read Dead Redemption 2 | D3DMetal / Mullion | 
| Windows Installers | Hesi Launcher | D3DMetal / Mullion |
| Windows Installers | Moonscraper Chart Editor | Mullion |


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
| Assassin's Creed Odyssey | 812140 | |
| Eve Online | 8500 | Launches through steam with Specific Flags that enable the launcher to load |
| Far Cry 5 | Ubisoft | |
| Far Cry 6 | Ubisoft | |
| TrackMania | Ubisoft | |
| Marvel Rivals | 2767030 | Launches through Steam with -Windowed to avoid a mouse bug |
| Baldur's Gate 3 | 1086940 | |
| Watch Dogs Legions | Ubisoft | |
| Red Dead Redemption 2 | 1174180 | |
| Grand Theft Auto V Enhanced | 3240220 | Story mode |
| Deadlock | 1422450 | Online Play |
| Assassins Creed - Black Flag Resented | 3751950 | Long First-Boot |
| God of War ; Ragnorock | 2322010 | |
| Control Resonant | 3669870 | |
| Counter Strike 2 | 730 | |
| Assetto Corsa | 244210 | |
| Peak | 3527290 | |
| Starfield | 716740 | |
| God Of War | 1593500 | |
| Inzoi | 2456740 | | 
| Control Ultimate Edition | 870780 | |
| Marvel Rivals | 2767030 | |
| Eve Online | 8500 | |
| Spider Man: Remastered | 1817070 | |
| Dragon's Dogma 2 | 2054970 | |
| Risk of Rain 2 | 632360 | |



---

## VKD3D/DXVK - D3D12, D3D11, D3D10, D3D9 -> WineVulkan -> MoltenVK -> Metal

| Game | AppID | Notes |
|---|---:|---|
| Limbus Company | 1973530 | |
| PEAK | 3527290 | Working | |
| Portal 2 | 620 | |
| 7 Days to Die | 251570 | |
| Hollow Knight: Silksong | 1030300 | |
| Team Fortress 2 | 440 | |
| Schedule I | 3164500 | |
| Dark Deception | 332950 | |
| Mirror's Edge | 17410 | Sync-loading mitigation active. |
| Half-Life 2 | 220 | |
| Among Us | 945360 | Steam online play. |
| Dwarf Fortress | 975370 | |
| Caves Of Qud | 333640 | |
| Skyrim: Special Edition | 489830 | |

---

## Mullion (DXMT) — D3D12, D3D11, D3D10 -> Metal (x64, x86)

Plays most of the games D3DMetal can play alongside the following; 

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
| Team Fortress 2 | 440 | x87 acceleration |
| Amid Evil | 673130 | |
| Octopath Traveler II | 1971650 | |
| Mind Scanners | 1389550 | |
| Dredge | 1562430 | |
| Blasphemous 2 | 2114740 | |
| Ori and the Blind Forest; Definitive Edition | 387290 | x87 acceleration |
| Nidhogg 2 | 535520 | |
| Hades | 1145360 | |
| Undertale | 391540 | |


---

## OpenGL - OpenGL 1.0 -> 4.6 through WineMetalGL -> Metal

| Game | AppID | Notes |
|---|---:|---|
| Doom (2016) | 379720 | |
| Doom 3, Doom 3: BFG Edition | 208200 | |
| Quake 4 | 2210 | |
| Wolfenstein; The New Order | 201810 | |
| The Elder Scrolls III; Morrowind with OpenMW | 22320 | |
| Soma | 282140 | |
| Portal | 400 | |
| Eduke 32 | | |
| Star Wars Jedi Knight; Jedi Academy | 6020 | |
| Cassette Beasts | 1321440 | |
| Foundation | 690830 | |
| Minecraft | Prism, Modrinth, and Base Launchers | Supports Shaders up to 4.6 Context |

---
