<div align="center">

<p align="center">
  <img width="200" alt="MetalSharp X Wine" src="https://github.com/user-attachments/assets/f9c077b2-fd1f-4d56-9c58-7cf9a64e7c12" />
</p>

___

### Run Windows games on MacOS Silicon.

<a href="https://github.com/metalsharp/MetalSharp/releases/tag/v1.0.0"><img src="https://img.shields.io/github/v/release/metalsharp/MetalSharp?filter=v1.0.0&style=for-the-badge" alt="Release"></a>
<a href="https://github.com/metalsharp/MetalSharp/releases"><img src="https://img.shields.io/github/downloads/metalsharp/MetalSharp/total?style=for-the-badge" alt="Downloads"></a>
<a href="LICENSE"><img src="https://img.shields.io/badge/License-AGPL--3.0-purple.svg?style=for-the-badge" alt="License"></a>
<a href="https://discord.gg/kQfFNvVWt"><img src="https://img.shields.io/badge/Discord-Join%20Chat-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord"></a>

</div>

---

<div align="center">
  
MetalSharp is a Free Wine 11.17 x86_64 (MacOS 15+) / Wine 11.20 with FEX (MacOS 26.4+) Custom-Built Runtime That Runs Windows Applications / Games on Apple Silicon, Built Inside a GPUI Application. MetalSharp Aims to Make The Process of Launching Your Games Easy Without Extra Needed Configuration Steps. 

<img width="1090" height="819" alt="Screenshot 2026-10-27 at 9 42 50 PM" src="https://github.com/user-attachments/assets/7e5f78e9-d406-4d07-bfde-e1f1d3339f77" />



</div>

## Quick Start

Download the DMG from [Releases](https://github.com/metalsharp/MetalSharp/releases), drag MetalSharp into `/Applications`, and open it. Each release has two builds:

- `MetalSharp-<version>-arm64.dmg`: the x86_64 Wine build under Rosetta (MacOS 15+).
- `MetalSharp-<version>-arm64-FEX.dmg`: the Arm64 build with FEX and MetalSharp Wine (MacOS 26.4+), in early access.

The in-app updater keeps the build you have, and can switch between them.

The setup wizard handles installing the remaining runtime dependencies. Installed games automatically sync into the User Library.

Alternatively, Install MetalSharp from the official **_Homebrew_** tap:

```bash
brew install --cask metalsharp/tap/metalsharp
```

Homebrew installs MetalSharp.app in `/Applications`. Updates are handled in-app with the included updater. 


For Direct Access to What is Installed During Setup, See Here: [Dependency Bundles](https://github.com/metalsharp/MetalSharp/releases/tag/bundles).

If an In-App update fails, please use [Fix Broken Updater](https://github.com/metalsharp/MetalSharp/blob/main/docs/Fix%20a%20Broken%20Update.md)

## Graphics Routes

- To launch your game, save your desired pipeline in MetalSharp and launch through MetalSharp.
- MetalSharp automatically assigns your game a pipeline based on [Game-Compatibility](https://github.com/metalsharp/MetalSharp/blob/main/docs/games-supported.md) testing. Each build has its own rule set (`configs/mtsp-rules.toml`, `configs/mtsp-rules-fex.toml`).

| Version | Route | Engine | Notes |
|---|---|---|---|
| Arm64 FEX | **_VKD3D-Proton_** | VKD3D-Proton D3D12 Through Vulkan -> MetalSharp's MoltenVK  -> Metal | In Early-Access |
| Arm64 FEX | **_Mullion (DXMT Fork)_** | D3D12 / D3D11 / D3D10 x86 / x64 -> Metal | In Early-Access |
| Arm64 FEX | **_DXVK_** | D3D11 / D3D10 / D3D9 -> Vulkan -> MetalSharp's MoltenVK -> Metal | |
| Arm64 FEX | **_OpenGL_** | WineMetalGL (OpenGL 1.0 through 4.6) -> Metal | | 

___

| Version | Route | Engine |
|---|---|---|
| x86_64 | **_D3DMetal_** | D3D12/11/10 Through the Latest D3DMetal Framework |
| x86_64 | **_VKD3D_** | D3D12 Through VKD3D-Proton, D3D11 / D3D10 / D3D9 Through DXVK -> MoltenVK -> Metal |
| x86_64 | **_DXMT_** | D3D11, D3D10 to Metal |
| x86_64 | **_DXMT(32)_** | D3D11, D3D10 32-bit to Metal |
| x86_64 | **_D3D9_** | D3D9 With x87 Acceleration for Older Titles |
| x86_64 | **_Mono/FNA_** | Windows XNA/FNA via Native Mono |

## Features

| Feature | Notes |
|---|---|
| **_Sharp Library_** | Import and run standalone Windows programs, installers, and launchers |
| **_GOG Library_** | Download and play GOG games through the Sharp Library |
| **_Epic Library_** | Download and play Epic Games through the Sharp Library |
| **_Ubisoft Connect_** | Download and play Ubisoft Connect Games |
| **_GameJolt Library_** | Download and Play GameJolt games through the Sharp Library |
| **_Emulation Support_** |  Install, manage, and launch emulated games using PCSX2, RPCS3, ShadPS4, and SharpEmu |
| **_Runtime Bottles_** | Select your launch method, Enable / Disable Msync and MetalFX; Controller Input and x87 Acceleration on x86_64; Metal HUD, Frame Rate Limits and Ray Tracing on Arm64 FEX |
| **_Route Routing_** | Automatic route selection based on game compatibility data and developer testing |
| **_Steam Integration_** | Detects your Steam library, manages the Wine Steam session, and deploys a CEF runtime wrapper that survives Steam updates |
| **_Steam Art Manager_** | Set custom artwork for any game in your Steam-Library(s) |
| **_Game Streaming_** | Streams games to a phone or tablet with Sunshine + Moonlight |

## Requirements

For x86_64 Version Under Rosetta: 

- Apple Silicon Mac M1-M5, MacOS 15+
- About ~12 GB free space

For Arm64 Version with FEX (MetalSharp Wine 4.0): 

- Apple Silicon Mac M1-M5, MacOS 26.4+
- About ~12GB free space 

## Developer Setup

Current maintainer validation is happening on this hardware/software setup. This is not the recommended baseline or minimum requirement; it is here so readers know what MetalSharp is actively running on during development.

- Apple M4 Macbook Air, 10-core CPU (4 performance, 6 efficiency)
- 16 GB memory
- macOS Golden Gate, Version `27.2`

## Documentation

- [Development](docs/development.md)
- [User Guide](docs/user-guide.md)
- [Architecture](docs/architecture.md)
- [Ubisoft Connect Guide](docs/ubisoft-connect.md)
- [All Docs](docs/README.md)

## License

Licensed under the `GNU Affero General Public License v3.0` or later (AGPL-3.0-or-later). Third-party components keep their original licenses; see [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).

## Special Thanks To; 

[DXMT](https://github.com/3Shain/dxmt) _For DirectX -> Metal_,  [Wine](https://www.winehq.org/) _For Tiny Windows VM_, 
[FEX-Emu](https://github.com/FEX-Emu/FEX) _For x86 on Arm64_, 
[VKD3D-Proton](https://github.com/HansKristian-Work/vkd3d-proton) _For D3D12 on Vulkan_, 
[MoltenVK](https://github.com/KhronosGroup/MoltenVK) _For Vulkan on Metal_, 
[DXVK-MacOS](https://github.com/Gcenx/DXVK-macOS) _For Vulkan On Mac_, 
[Tormak9970](https://github.com/Tormak9970/Steam-Art-Manager) _For Steam Art Manager_, 
[ZED](https://github.com/zed-industries/zed) _For GPUI_, 
[Wine-Mono](https://github.com/wine-mono/wine-mono), 
[Legendary](https://github.com/legendary-gl/legendary) _For Headless Epic Login_, 
[Heroic](https://github.com/Heroic-Games-Launcher/HeroicGamesLauncher) _For Headless GOG Login_, and [WineForge](https://github.com/Alien4042x/WineForge) _For WFDXCompat / WFUSync_

Without their work, this project would not have been possible. 

## Hall Of Fame; 

Users who have contributed to MetalSharp in it's active development:

[TimDarcet](https://github.com/TimDarcet) , [erenaspire7](https://github.com/erenaspire7) , [eth0-re](https://github.com/eth0-re) , [MohamedElGhandour](https://github.com/MohamedElGhandour)

## Star History

<a href="https://star-history.com/#metalsharp/MetalSharp&Date">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=metalsharp/MetalSharp&type=Date&theme=dark" />
    <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=metalsharp/MetalSharp&type=Date" />
    <img alt="Star history chart for metalsharp/MetalSharp" src="https://api.star-history.com/svg?repos=metalsharp/MetalSharp&type=Date" />
  </picture>
</a>
