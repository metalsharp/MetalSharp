<div align="center">

# MetalSharp
**Updated:** 2026-07-08

**Run Windows games on macOS Silicon with Metal.**

<a href="https://github.com/aaf2tbz/metalsharp/actions"><img src="https://img.shields.io/github/actions/workflow/status/aaf2tbz/metalsharp/ci.yml?branch=main&style=for-the-badge" alt="CI"></a>
<a href="https://github.com/metalsharp/MetalSharp/releases/tag/v0.57.0"><img src="https://img.shields.io/github/v/release/aaf2tbz/metalsharp?filter=v0.57.0&style=for-the-badge" alt="Release"></a>
<a href="https://github.com/aaf2tbz/metalsharp/discussions"><img src="https://img.shields.io/github/discussions/aaf2tbz/metalsharp?style=for-the-badge" alt="Discussions"></a>
<a href="LICENSE"><img src="https://img.shields.io/badge/License-PolyForm%20Noncommercial-yellow.svg?style=for-the-badge" alt="PolyForm Noncommercial 1.0.0"></a>
<a href="https://discord.gg/qW5rUr4dH"><img src="https://img.shields.io/badge/Discord-%235865F2.svg?style=for-the-badge&logo=discord&logoColor=white" alt="DISCORD"></a>

</div>

---

MetalSharp is an application designed to run Windows Steam and Windows Steam games natively on Apple Silicon macOS. Now includes GOG-Games Support. MetalSharp builds and includes its own custom Wine 11.5 runtime, game launch rules, custom DXMT build, runtime bottles, and repair tooling. 

<img width="946" height="646" alt="Screenshot 2026-07-21 at 5 49 12 AM" src="https://github.com/user-attachments/assets/c4aa6d64-ca4b-4ee2-ade4-5f12d511925a" />

## Arm64-FEX Build For Devs Out Now

[Releases](https://github.com/metalsharp/MetalSharp/releases/tag/v0.60.0-dependency-bundles), 0.57.0 Remains The Current Working Version. 

## Quick Start

Download the latest DMG from [Releases](https://github.com/aaf2tbz/metalsharp/releases), drag MetalSharp into `/Applications`, and open it. The setup wizard handles the rest. To ensure accurate Steam Installation, please wait for Steam to finish both update windows it will launch before installing the x64 /x86 Reddist Installers. 

If Gatekeeper cannot verify the app, open **System Settings → Privacy & Security** and choose **Open Anyway** for MetalSharp. VirusTotal scans are included with release notes when available.

### Install With Homebrew:

```sh
brew install --cask aaf2tbz/tap/metalsharp
```

This installs the signed MetalSharp release into `/Applications`. For developer builds, see [Build from Source](docs/guides/install-from-source.md).

## Routes

| Route | Engine | How To Use |
|---|---|---|
| **M12** | D3D12 to Metal (experimental DXMT) | Not yet playable |  
| **M11** | D3D11 to Metal (DXMT) | Save 'M11' Bottle, Hit Play |
| **M11(32)** | D3D11 i386 to Metal (DXMT) | Save M11(32) Bottle, Hit Play |
| **M10** | D3D10 to Metal (DXMT) | Save 'M10' Bottle, Hit Play |
| **M10(32)** | D3D10 i386 to Metal (DXMT) | Save M10(32) Bottle, Hit Play |
| **M9** | D3D9 i386 Wine, DXMT Overrides| Save 'M9' Bottle, Hit Play |
| **Mono/FNA** | Windows XNA/FNA via native Mono | Save 'Mono/Fna' Bottle, Hit Play |
| **D3DMetal** | Apple Game Porting Toolkit via Homebrew. GPTK is not bundled; selecting a D3DMetal bottle installs/trusts Homebrew GPTK + Rosetta, then seeds the GPTK prefix with Homebrew-matched D3DMetal route DLLs. | Save 'D3DMetal' Bottle, Fix, Hit Play |

## Features and Notes 

|---|---|
| **Sharp Library** - Import and run standalone Windows programs, installers, and launchers | Working Partially |
| **Sharp GOG Library** - Download and play GOG games througn the sharp library | Prefix, Sign in, Play |
| **Runtime Bottles** - Select your launch method, repair missing assets, and switch between bottle runtimes | Good For Running Games |
| **MTSP Routing** - Automatic pipeline selection based on game compatibility data and developer testing | Default Rules for Tested Games |
| **Steam Integration** - Detects your Steam library, manages the Wine Steam session, and deploys a CEF runtime wrapper that survives Steam updates | |

## Requirements

- Apple Silicon Mac M1-M5, macOS 14+
- About 2 GB free space
- Homebrew (installed by setup wizard)

All other bundled assets, DLLs, and graphics backends are installed during the setup process. GPTK/D3DMetal is the exception: MetalSharp installs and uses Homebrew GPTK only when a D3DMetal bottle is saved.

## Developer Setup

Current maintainer validation is happening on this hardware/software setup. This is not the recommended baseline or minimum requirement; it is here so readers know what MetalSharp is actively running on during development.

- Apple M4 Macbook Air, 10-core CPU (4 performance, 6 efficiency)
- 16 GB memory
- macOS Golden Gate beta, version 27.0

## Documentation

- [Build from Source](docs/guides/install-from-source.md)
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

MetalSharp-owned code is licensed under the [PolyForm Noncommercial License
1.0.0](LICENSE). Commercial licensing is available from
`averyfelts@aol.com`. Third-party components keep their original licenses;
see [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).
