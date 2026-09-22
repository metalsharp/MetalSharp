# SharpEmu Managed Environment
**Updated:** 2026-09-22

MetalSharp runs SharpEmu as an experimental PlayStation 5 research environment in the Sharp Library. Most games do not run; macOS support is experimental. SharpEmu is GPL-2.0-or-later and runs out of process.

## Managed Layout

```text
~/.metalsharp/emulators/sharpemu/
├── current -> versions/<release-tag>
├── previous -> versions/<release-tag>
├── versions/<release-tag>/     # read-only: SharpEmu, libMoltenVK.dylib, libvulkan.1.dylib,
│                               # plugins/, licenses/, LICENSE.txt, manifests
├── home/                       # isolated HOME
├── state/saves/  state/custom-configs/  state/roots.json
├── cache/dotnet-bundle/  cache/ampr-index/  cache/vulkan/
├── writable/
├── downloads/  staging/  sessions/  logs/
├── environment.json  update-policy.json  library-cache.json
```

Activated version trees are read-only. Updates, rollback, repair, and runtime removal preserve saves, settings, roots, caches, logs, sessions, and external games.

## Host Requirements

- The official macOS release is x86-64; Apple Silicon uses Rosetta 2, Intel runs it directly.
- macOS 26+ (bundled FFmpeg dylibs declare `minos 26.0`).
- `lsar` and `unar`; at least 1 GiB of transaction space.
- Vulkan uses the bundled MoltenVK.

## Installation and Updates

MetalSharp downloads only the exact official stable macOS x64 asset from `sharpemu/sharpemu` after user confirmation. The frozen research baseline: release `v0.0.3-release.3`, source commit `d9b599a1`, asset `sharpemu-0.0.3-release.3-osx-x64.tar.gz` (71,999,495 bytes, SHA-256 `cf54f8f5…d05b681a`, 31 entries, 111,974,175 extracted bytes, self-contained .NET 10).

GitHub reports the asset as mutable, so MetalSharp binds release ID, asset ID, tag, name, URL, size, digest, and timestamps; changed metadata for an observed tag/asset is quarantined rather than treated as an update.

Transaction: bounded release JSON → non-draft stable `v...` tag → exactly one `sharpemu-<version>-osx-x64.tar.gz` → `.part` download with HTTPS-only redirects → exact bytes/SHA-256 → `lsar` preflight (rejects traversal, links, devices, sparse entries, duplicates, case collisions, bounds) → `unar` extract into same-volume staging → require executable, Vulkan loaders, plugins, licenses → inspect every Mach-O → record pre-sign hashes in `source-manifest.json` → ad-hoc sign locally (the upstream archive is unsigned; Gatekeeper rejects it as-is) → verify signatures and MoltenVK loading plus the nonexistent-eboot CLI probe → record post-sign hashes in `activation-manifest.json` → read-only version tree → atomic `current` switch with `previous` retained.

Users can check/refresh metadata, install/update, pin, unpin, skip, clear skip, roll back, and remove runtime versions. Download may run during play; activation waits for sessions to exit.

## Game Discovery

Roots are picked with the native directory picker; symlinked, missing, system, and MetalSharp-managed roots are rejected, along with duplicates/overlaps and more than 32 roots. Scanning finds exact regular `eboot.bin` files (depth 8, 20,000 entries, 512 games), validates bounded ELF/fSELF structure, reads at most 1 MiB of `param.json` for title, PPSA title ID, and versions, and validates local PNG artwork. Launch reopens the executable no-follow and compares size to the scan identity; changed files require a rescan. Removing a root never deletes external content.

## Launch

CLI-only, no upstream GUI or updater:

```text
--cpu-engine=native --log-level=info --log-file <isolated-log> \
--window-mode=windowed --scaling=fit --vsync=on <canonical-eboot.bin>
```

The child environment redirects `HOME`, .NET extraction, saves, AMPR indexes, the Vulkan pipeline cache, guest mounts, `TMPDIR`, and logs into the managed tree, and clears inherited debugger/profiler/proxy/RenderDoc/diagnostic variables.

Executable content: a decrypted ELF or a recognized fake-signed SELF; MetalSharp validates bounded leading structure only. Valid PS5 compatibility IDs match `PPSA` plus five digits. Local artwork uses `sce_sys/icon0.png`, then `pic0.png`, `pic1.png` after PNG checks.

## Guest Networking

SharpEmu guest networking maps to real host sockets. Default launches run through `sandbox-exec` with all network operations denied; readiness verifies the sandbox blocks a loopback connection, and launches fail closed when containment is unavailable. An explicit "Allow unrestricted guest networking" checkbox with a persistent danger state and per-launch second confirmation enables networking; session records store `networkEnabled`.

## Process Supervision

Each launch gets a session ID, PID/process group, exact executable identity, canonical game path, log path, start timestamp, and network policy. Backend restart recovery validates command path and start time. Stop sends SIGINT → bounded wait → SIGTERM → SIGKILL. Exit status and the latest log stay on the game card.

## API

```text
GET  /emulators, /sharp-library/sharpemu/{status,games,cover,sessions,update/check,update/progress}
POST /sharp-library/sharpemu/{scan,add-root,remove-root,launch,stop,update/refresh,update/install,
     update/rollback,pin-current,unpin,skip-update,clear-skip,remove-runtime}
```

The preload exposes bounded backend requests, the game-root picker, contained path reveals, and the exact official FAQ/compatibility URLs.

## Maintenance Procedure

Before allowlisting a new stable release: freeze tag and source commit; record release/asset IDs, URL, size, digest, timestamps, mutability; audit archive paths and byte bounds; record Mach-O architectures, deployment targets, install names, dependencies; confirm CLI parsing and nonexistent-path exit behavior; re-audit writable env vars, diagnostic/network toggles, and sandbox behavior; run the full test suites; update this contract, `THIRD_PARTY_LICENSES`, and the capability manifest. Keep the release unavailable if any contract changed without a fail-closed adaptation.
