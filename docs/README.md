# MetalSharp Docs
**Updated:** 2026-09-22

Use this page as the repo map before changing launch/runtime code.

## Guides

- [How to Use MetalSharp](guides/how-to-use-metalsharp.md) - install, launch, diagnose, and update flow.
- [Game Streaming](guides/game-streaming.md) - stream games to a phone or tablet with Sunshine + Moonlight.
- [Install from Source](guides/install-from-source.md) - build MetalSharp from source without the DMG.
- [How to Build MetalSharp Wine](guides/how-to-build-metalsharp-wine.md) - Wine 11.17 source prerequisites, tools, dependencies, and WoW64 build commands.

## Compatibility

- [Supported Games](compatibility/GAMES-SUPPORTED.md) - tested games per launch route with notes.

## Runtime

- [Runtime Bundles and Steam Routing](runtime/runtime-bundles-and-steam-routing.md) - bundle provenance, wrapper deployment, and the Wine Steam route.
- [Mono Runtime Lanes](runtime/mono-runtime-lanes.md) - Mono/FNA route lanes and shim staging.
- [Wine Architecture](runtime/wine-architecture.md) - Wine 11.17, shared Steam prefixes, bundled D3DMetal, and wrapper behavior.
- [Launcher Runtime](runtime/launcher-runtime.md) - Sharp Library launcher and CEF/WebView runtime handling.
- [Compatdata Architecture](runtime/compatdata-architecture.md) - Steam game compatdata ownership.
- [Host Runtime ABI](runtime/host-runtime-abi.md) - host shim ABI boundaries.
- [Redistributable Runtime](runtime/redistributable-runtime.md) - redistributable source and repair policy.
- [Steam Compatibility Tool Surface](runtime/steam-compatibility-tool-surface.md) - Steam-facing compatibility contract.
- [Vendor Trust Kit](runtime/vendor-trust-kit.md) - vendor runtime evidence and trust boundaries.
- [Host Shim Inventory](runtime/metalsharp-host-shim-inventory.md) - current host/runtime shim inventory.
- [Darwin Sync Map](runtime/darwin-sync-map.md) - macOS synchronization primitive map.
- [WineMetalGL Port](runtime/winemetalgl-wine-11.17-port.md) - OpenGL support in the Wine 11.17 runtime.

## Architecture

- [Launch Architecture](architecture/launch-architecture.md) - route resolution and launch ownership.
- [Graphics Routes](architecture/graphics-routes.md) - the five launch routes, DLL sets, and cache paths.
- [D3D12 Developer Runtime Package](../tools/d3d12-metal-sdk/docs/developer-runtime.md) - self-contained developer SDK tarball layout and publish flow.

## Release

- [Release Signing](release/release-signing.md)

## Bundle Truth Sources

- Release assets live on the [`bundles` GitHub release](https://github.com/aaf2tbz/metalsharp/releases/tag/bundles).
- Manifest-tracked hashes live in [tools/bundles/asset-manifest.tsv](../tools/bundles/asset-manifest.tsv).
- Verify local and remote bundle state with:

```bash
tools/bundles/verify-bundles.sh --release
tools/bundles/verify-bundles.sh --require mac
```
