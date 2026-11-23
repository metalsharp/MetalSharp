<div align="center">

<p align="center">
  
<img width="300" alt="MetalSharp X MetalSharp Wine" src="https://github.com/user-attachments/assets/59a8cd89-f8d5-41eb-ba1e-2bb5839ef8d6" />

</p>

<a href="https://github.com/metalsharp/MetalSharp/releases/tag/v1.5.0"><img src="https://img.shields.io/github/v/release/metalsharp/MetalSharp?filter=v1.5.0&style=for-the-badge" alt="Release"></a>
<a href="https://github.com/metalsharp/MetalSharp/releases"><img src="https://img.shields.io/github/downloads/metalsharp/MetalSharp/total?style=for-the-badge" alt="Downloads"></a>
<a href="LICENSE"><img src="https://img.shields.io/badge/License-AGPL--3.0-purple.svg?style=for-the-badge" alt="License"></a>
<a href="https://discord.gg/kQfFNvVWt"><img src="https://img.shields.io/badge/Discord-Join%20Chat-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord"></a>

</div>

---

<div align="center">

<img width="1415" height="954" alt="MetalSharp Library" src="https://github.com/user-attachments/assets/7aff8426-ff4f-498c-91a5-2495eb2e3361" />

</div>

## Quick start: 

MetalSharp ships two different runtimes, both using the same graphics engines; 

- X86_64 Wine 11.17 under Rosetta 2, for MacOS 15+
- Arm64/Arm64EC Wine 11.20 with FEX, for MacOS 26.4+

Download your preferred DMG, and run it. Move MetalSharp into the applications folder, and launch it. Setup sets up everything you need, and you should install Steam from here to create your main prefix, or 'bottle'. That's it! Optionally, you can switch between both builds, and run games under both versions. 

## Graphics Routes

- To launch your game, save your desired pipeline in MetalSharp and launch through MetalSharp.
- MetalSharp automatically assigns your game a pipeline based on [Game-Compatibility](https://github.com/metalsharp/MetalSharp/blob/main/docs/games-supported.md) testing.

| Route | Engine |
|---|---|
| **_VKD3D/DXVK_** | VKD3D-Proton D3D12, DXVK D3D11/10/9 Through Vulkan -> MetalSharp's MoltenVK  -> Metal |
| **_Mullion (DXMT Fork)_** | D3D12 / D3D11 / D3D10 -> Metal |
| **_OpenGL_** | WineMetalGL (OpenGL 1.0 through 4.6) -> Metal |
| **_MetalVR_** | Play VR Games Through Many Headsets With D3D11 / 12 |
| **_D3DMetal_** | D3D12/11/10 Through Apple's D3DMetal. Only Available On The x86_64 Version |


## Features

| Feature | Notes |
|---|---|
| **_Windows Programs_** | Import and Run Standalone Windows Programs, Installers, and Launchers |
| **_Steam Integration_** | Detects your Steam library, manages the Wine Steam session. Set Custom Artwork Images For Any Game's Hero / App Card |
| **_Launcher Support_** | Supports GOG, Ubisoft Connect, Battle.Net, Rockstar, and Epic Games |
| **_Per-Game Settings_** | Select Launch Method, MetalHUD Options, x87 Acceleration, Frame-rate Limits, And More  |
| **_VR Support_** | Launch Games With SteamVR For Oculus CV1/S, PSVR2, Quest 2/3/3s/Pro, Valve Index, and HTC Vive | 
| **_Game Streaming_** | Streams games to a phone or tablet with Sunshine + Moonlight |

## Requirements

**_For x86_64 Version Under Rosetta:_**

- Apple Silicon Mac M1-M5, MacOS 15+
- About ~4 GB Free Space (Extra Prefixes, or 'Bottles' Take up More Space. 

**_For Arm64 Version with FEX (MetalSharp Wine 4.0):_**

- Apple Silicon Mac M1-M5, MacOS 26.4+
- About ~4 GB Free Space (Extra Prefixes, or 'Bottles' Take up More Space.

## Links / Docs

- [Tested And Verified Games](https://github.com/metalsharp/MetalSharp/blob/main/docs/games-supported.md)
- [MetalVR](https://github.com/metalsharp/MetalVR)
- [VKD3D-Proton MacOS](https://github.com/metalsharp/VKD3D-Proton-MacOS)
- [WineMetalGL](https://github.com/metalsharp/WineMetalGL)
- [Updates](https://github.com/metalsharp/MetalSharp/blob/main/docs/updates.md)

## License

Licensed under the `GNU Affero General Public License v3.0` or later (AGPL-3.0-or-later). Third-party components keep their original licenses; see [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).

## Special Thanks To These Projects, Which Enable a Lot of What MetalSharp Is / Does; 

[Mullion](https://github.com/dttdrv/mullion), 
[Wine](https://www.winehq.org/), 
[FEX-Emu](https://github.com/FEX-Emu/FEX), 
[VKD3D-Proton](https://github.com/HansKristian-Work/vkd3d-proton), 
[MoltenVK](https://github.com/KhronosGroup/MoltenVK), 
[DXVK-MacOS](https://github.com/Gcenx/DXVK-macOS), 
[Steam Art Manager](https://github.com/Tormak9970/Steam-Art-Manager), 
[GPUI](https://github.com/zed-industries/zed), 
[Wine-Mono](https://github.com/wine-mono/wine-mono), 
[Legendary](https://github.com/legendary-gl/legendary), 
[Heroic](https://github.com/Heroic-Games-Launcher/HeroicGamesLauncher),
[WineForge](https://github.com/Alien4042x/WineForge)

Without their work, this project would not have been possible. 

## Hall Of Fame; 

Users who have contributed to MetalSharp in it's active development:

[TimDarcet](https://github.com/TimDarcet) , [erenaspire7](https://github.com/erenaspire7) , [eth0-re](https://github.com/eth0-re) , [MohamedElGhandour](https://github.com/MohamedElGhandour), [renewrocha](https://github.com/renewrocha), [bondarev](https://github.com/bondarev), [ultima9](https://github.com/ultima9)
