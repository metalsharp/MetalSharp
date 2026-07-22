# MetalSharp Docs
**Updated:** 2026-07-08


Use this page as the repo map before changing launch/runtime code.

## Guides

- [How to Use MetalSharp](guides/how-to-use-metalsharp.md) - install, launch, diagnose, and update flow.
- [Library and Logs UI](guides/library-and-logs-ui.md) - library source selection, Sharp app tools, and log drawer behavior.
- [Install from Source](guides/install-from-source.md) - build MetalSharp from source without the DMG.
- [GPTK (D3DMetal) Guide](guides/gptk-guide.md) - Homebrew GPTK setup, D3DMetal bottle actions, prefix seeding, and troubleshooting.

## Compatibility

- [Supported Games](compatibility/GAMES-SUPPORTED.md) - current working/blocked game evidence and recommended public routes.
- [Proof Targets](compatibility/proof-targets.md) - local evidence targets and runtime proof notes.

## Runtime

- [Runtime Bundles and Steam Routing](runtime/runtime-bundles-and-steam-routing.md) - bundle provenance, wrapper deployment, and the correct Wine Steam route.
- [Mono Runtime Lanes](runtime/mono-runtime-lanes.md) - Mono/FNA/XNA route boundaries and native Mono lane details.
- [Wine Architecture](runtime/wine-architecture.md) - Wine prefix/runtime layout and wrapper behavior.
- [Launcher Runtime](runtime/launcher-runtime.md) - Sharp Library launcher and CEF/WebView runtime handling.
- [Compatdata Architecture](runtime/compatdata-architecture.md) - Steam game compatdata ownership.
- [Host Runtime ABI](runtime/host-runtime-abi.md) - host shim ABI boundaries.
- [Redistributable Runtime](runtime/redistributable-runtime.md) - redistributable source and repair policy.
- [Steam Compatibility Tool Surface](runtime/steam-compatibility-tool-surface.md) - Steam-facing compatibility contract.
- [Vendor Trust Kit](runtime/vendor-trust-kit.md) - vendor runtime evidence and trust boundaries.
- [Host Shim Inventory](runtime/metalsharp-host-shim-inventory.md) - current host/runtime shim inventory.
- [Darwin Sync Map](runtime/darwin-sync-map.md) - macOS runtime sync notes.

## Architecture

- [Launch Architecture](architecture/launch-architecture.md) - pipeline selection and launch ownership.
- [D3D12 Pipeline Map](architecture/m12-pipeline-map.md) - current M12 D3D12/DXMT path.
- [D3D10 Pipeline Map](architecture/m10-pipeline-map.md) - current M10 D3D10/DXMT path.
- [D3D9 Pipeline Map](architecture/m9-pipeline-map.md) - current M9 D3D9 route.
- [DXMT and Vulkan Architecture](architecture/dxmt-vulkan-architecture.md) - DXMT/M9-M12 boundaries and Vulkan fallback boundaries.
- [D3D12 Developer Runtime Package](../tools/d3d12-metal-sdk/docs/developer-runtime.md) - self-contained developer SDK tarball layout and publish flow.

## Historical Roadmaps

These files are planning history and should not be treated as the current implementation contract without checking the code and runtime manifests first. Archived roadmaps have moved to `docs/archive/roadmaps/`.

### Active roadmaps (current planning)

- [DXMT Proton-Parity Roadmap](roadmaps/dxmt-proton-parity-roadmap.md)
- [Installer Runtime Roadmap](roadmaps/installer-runtime-roadmap.md)
- [Anti-Cheat Hard Route Roadmap](roadmaps/anticheat-hard-route-roadmap.md)
- [D3D12 Offline ABI Surface Matrix](roadmaps/d3d12-offline-abi-surface-matrix.md)
- [D3D12 Offline Cache Preflight](roadmaps/d3d12-offline-cache-preflight.md)
- [D3D12 Offline Gate Results](roadmaps/d3d12-offline-gate-results.md)

### Archived roadmaps (historical, completed work)

See [archive README](archive/README.md) for the full list. Do not cite archived roadmaps as current contracts.

## Research

- [Proton Runtime Research](research/proton-runtime-research.md)
- [Anti-Cheat Compatibility Boundaries](research/anti-cheat-compatibility-boundaries.md)

## Release

- [Release Signing](release/release-signing.md)

## Bundle Truth Sources

- Release assets live on the [`bundles` GitHub release](https://github.com/aaf2tbz/metalsharp/releases/tag/bundles).
- Manifest-tracked hashes live in [tools/bundles/asset-manifest.tsv](../tools/bundles/asset-manifest.tsv).
- GPTK/D3DMetal is not a MetalSharp bundle asset. D3DMetal uses Homebrew GPTK at `/Applications/Game Porting Toolkit.app` and seeds matched route DLLs into `~/.metalsharp/prefix-gptk` when a D3DMetal bottle is prepared.
- Verify local and remote bundle state with:

```bash
tools/bundles/verify-bundles.sh --release
tools/bundles/verify-bundles.sh --require mac
```
