<div align="center">

# MetalSharp
**Updated:** 2026-07-18 

**Run Windows games on MacOS Silicon.**

<a href="https://github.com/aaf2tbz/metalsharp/actions"><img src="https://img.shields.io/github/actions/workflow/status/aaf2tbz/metalsharp/ci.yml?branch=main&style=for-the-badge" alt="CI"></a>
<a href="https://github.com/metalsharp/MetalSharp/releases/tag/v0.61.0"><img src="https://img.shields.io/github/v/release/metalsharp/MetalSharp?filter=v0.61.0&style=for-the-badge" alt="Release"></a>
<a href="https://github.com/aaf2tbz/metalsharp/discussions"><img src="https://img.shields.io/github/discussions/aaf2tbz/metalsharp?style=for-the-badge" alt="Discussions"></a>
<a href="LICENSE"><img src="https://img.shields.io/badge/License-PolyForm%20Noncommercial-purple.svg?style=for-the-badge" alt="License"></a>
<a href="https://discord.gg/qW5rUr4dH"><img src="https://img.shields.io/badge/Discord-%235865F2.svg?style=for-the-badge&logo=discord&logoColor=white" alt="DISCORD"></a>

</div>

---

MetalSharp is an application designed to run Windows Steam and Windows Steam games natively on Apple Silicon macOS. MetalSharp builds and includes its own custom Wine 11.5 runtime, game launch rules, custom DXMT build, runtime bottles, and repair tooling.



<br><img width="1012" height="881" alt="Screenshot 2026-08-17 at 7 12 37 PM" src="https://github.com/user-attachments/assets/e852a551-7c31-4efb-973f-a6b344e4de4b" /></br>




## Quick Start

Download the latest DMG from [Releases](https://github.com/metalsharp/MetalSharp/releases), drag MetalSharp into /Applications, and open it.

If Gatekeeper cannot verify the app, open **_System Settings → Privacy & Security_** and choose **_Open Anyway_** for MetalSharp. VirusTotal scans are included with release notes when available.

Alternatively, Install MetalSharp from the official **_Homebrew_** tap:

```bash
brew install --cask metalsharp/tap/metalsharp
```

Homebrew installs MetalSharp.app in /Applications. Open it and the setup wizard handles the remaining runtime dependencies.

For building from source, see [Install from Source](docs/guides/install-from-source.md).

## Routes - _Graphics Routes are intentionally separated to avoid launch conflicts_

| Route | Engine | Notes |
|---|---|---|
| **_VKD3D_** | D3D12/D3D11/D3D10/D3D9 through Vulkan | D3D12 Still In Development |
| **_M11_** | D3D11 to Metal (DXMT) | |
| **_M11(32)_** | D3D11 32Bit to Metal (DXMT) | |
| **_M10_** | D3D10 to Metal (DXMT) | |
| **_M10(32)_** | D3D10 32Bit to Metal (DXMT) | |
| **_M9_** | D3D9 To Metal, DXMT Overrides | |
| **_Mono/FNA_** | Windows XNA/FNA via native Mono | |
| **_D3DMetal_** | Apple Game Porting Toolkit via Homebrew. GPTK is not bundled; selecting a D3DMetal bottle installs/trusts Homebrew GPTK + Rosetta, then seeds the GPTK prefix with the game. | Optional upgrade to GPTK 3, Requires Steam-Emu to Play |

## Features

| Feature | Notes |
|---|---|
| **_Sharp Library_** | Import and run standalone Windows programs, installers, and launchers |
| **_GOG Library_** | Download and play GOG games through the Sharp Library |
| **_Epic Library_** | Download and play Epic Games through the Sharp Library |
| **_GameJolt Library_** | Download, Manage, and Play GameJolt games through the Sharp Library |
| **_Emulation Support_** |  Install, manage, and launch emulated games using PCSX2, RCPS3, ShadPS4, and SharpEmu |
| **_Runtime Bottles_** | Select your launch method, repair missing assets, and switch between bottle runtimes |
| **_MTSP Routing_** | Automatic pipeline selection based on game compatibility data and developer testing |
| **_Steam Integration_** | Detects your Steam library, manages the Wine Steam session, and deploys a CEF runtime wrapper that survives Steam updates |

## Requirements

- Apple Silicon Mac M1-M5, macOS 15+
- About 2 GB free space
- Homebrew (installed by setup wizard)

All other bundled assets, DLLs, and graphics backends are installed during the setup process. GPTK/D3DMetal is the exception: MetalSharp installs and uses Homebrew GPTK only when a D3DMetal bottle is saved.

## Developer Setup

Current maintainer validation is happening on this hardware/software setup. This is not the recommended baseline or minimum requirement; it is here so readers know what MetalSharp is actively running on during development.

- Apple M4 Macbook Air, 10-core CPU (4 performance, 6 efficiency)
- 16 GB memory
- macOS Golden Gate beta, version 27.0

## Documentation

- [Install from Source](docs/guides/install-from-source.md)
- [How to Use MetalSharp](docs/guides/how-to-use-metalsharp.md)
- [GPTK (D3DMetal) Guide](docs/guides/gptk-guide.md)
- [Game Compatibility](docs/compatibility/GAMES-SUPPORTED.md)
- [Launch Architecture](docs/architecture/launch-architecture.md)
- [Docs Map](docs/README.md)

## Community

- [Releases](https://github.com/aaf2tbz/metalsharp/releases)
- [Discussions](https://github.com/aaf2tbz/metalsharp/discussions)
- [Issues](https://github.com/aaf2tbz/metalsharp/issues)

## License

PolyForm Noncommercial 1.0.0 licensed. Third-party components keep their original licenses; see [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).
