<div align="center">

<h1><img src="docs/assets/metalsharp-transparent.png" alt="MetalSharp logo" width="40" height="40" /> MetalSharp</h1>

**Run Windows games on MacOS Silicon.**

<a href="https://github.com/metalsharp/MetalSharp/releases/tag/v0.73.0"><img src="https://img.shields.io/github/v/release/metalsharp/MetalSharp?filter=v0.73.0&style=for-the-badge" alt="Release"></a>
<a href="https://github.com/metalsharp/MetalSharp/releases"><img src="https://img.shields.io/github/downloads/metalsharp/MetalSharp/total?style=for-the-badge" alt="Downloads"></a>
<a href="LICENSE"><img src="https://img.shields.io/badge/License-PolyForm%20Noncommercial-purple.svg?style=for-the-badge" alt="License"></a>
<a href="https://discord.gg/qW5rUr4dH"><img src="https://img.shields.io/badge/Discord-%235865F2.svg?style=for-the-badge&logo=discord&logoColor=white" alt="DISCORD"></a>

</div>

---

<div align="center">
  
MetalSharp Is An Application Designed To Run Windows Applications and Windows Steam Games on Apple Silicon MacOS. Easily Select Between Different Launch Methods, Change Artwork For Games, And More. 

</div>






<img width="1249" height="877" alt="MetalSharp Library" src="https://github.com/user-attachments/assets/0989d116-1aea-4377-91b5-48e26e50badd" />







## Quick Start

Download the latest DMG from [Releases](https://github.com/metalsharp/MetalSharp/releases), drag MetalSharp into /Applications, and open it.

Alternatively, Install MetalSharp from the official **_Homebrew_** tap:

```bash
brew install --cask metalsharp/tap/metalsharp
```

Homebrew installs MetalSharp.app in /Applications. Open it and the setup wizard handles the remaining runtime dependencies.


For Direct Access to What is Installed During Setup, See Here: [Dependency Bundles](https://github.com/metalsharp/MetalSharp/releases/tag/bundles).

## Launching Games and Graphics Routes

To launch a game, install it through Steam and launch it through MetalSharp. Choose between the below backends from the bottle dropdown if the game does not originally run. MetalSharp scans the game folder for DLL's and automatically assigns it to a backend pipeline, tested games are automatically assigned their compatible pipeline from the `mtsp-rules.toml`. See [Game Compatibility](docs/compatibility/GAMES-SUPPORTED.md) for a list of tested and verified games.

| Route | Engine | Notes |
|---|---|---|
| **_D3DMetal_** | D3D12/11/10 Through the Latest D3DMetal Framework | |
| **_VKD3D_** | D3D12/D3D11/D3D10 Through Vulkan -> Metal | D3D12 still in development |
| **_DXMT_** | D3D11, D3D10 to Metal | |
| **_DXMT(32)_** | D3D11, D3D10 32-bit to Metal | |
| **_D3D9_** | D3D9 With x87 Acceleration for Older Titles | |
| **_Mono/FNA_** | Windows XNA/FNA via Native Mono | |

## Features

| Feature | Notes |
|---|---|
| **_Sharp Library_** | Import and run standalone Windows programs, installers, and launchers |
| **_GOG Library_** | Download and play GOG games through the Sharp Library |
| **_Epic Library_** | Download and play Epic Games through the Sharp Library |
| **_GameJolt Library_** | Download, Manage, and Play GameJolt games through the Sharp Library |
| **_Emulation Support_** |  Install, manage, and launch emulated games using PCSX2, RCPS3, ShadPS4, and SharpEmu |
| **_Runtime Bottles_** | Select your launch method, repair missing assets, and switch between bottle runtimes |
| **_Route Routing_** | Automatic route selection based on game compatibility data and developer testing |
| **_Steam Integration_** | Detects your Steam library, manages the Wine Steam session, and deploys a CEF runtime wrapper that survives Steam updates |
| **_Game Streaming_** | Streams games to a phone or tablet with Sunshine + Moonlight |

## Requirements

- Apple Silicon Mac M1-M5, MacOS 15+
- About 2 GB free space

## Developer Setup

Current maintainer validation is happening on this hardware/software setup. This is not the recommended baseline or minimum requirement; it is here so readers know what MetalSharp is actively running on during development.

- Apple M4 Macbook Air, 10-core CPU (4 performance, 6 efficiency)
- 16 GB memory
- macOS Golden Gate, Version 27.0

## Documentation

- [Install from Source](docs/guides/install-from-source.md)
- [How to Use MetalSharp](docs/guides/how-to-use-metalsharp.md)
- [Launch Architecture](docs/architecture/launch-architecture.md)
- [Docs Map](docs/README.md)

## Community

- [Releases](https://github.com/aaf2tbz/metalsharp/releases)
- [Discussions](https://github.com/aaf2tbz/metalsharp/discussions)
- [Issues](https://github.com/aaf2tbz/metalsharp/issues)

## License

PolyForm Noncommercial 1.0.0 licensed. Third-party components keep their original licenses; see [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).

## Star History

<a href="https://star-history.com/#metalsharp/MetalSharp&Date">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=metalsharp/MetalSharp&type=Date&theme=dark" />
    <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=metalsharp/MetalSharp&type=Date" />
    <img alt="Star history chart for metalsharp/MetalSharp" src="https://api.star-history.com/svg?repos=metalsharp/MetalSharp&type=Date" />
  </picture>
</a>
