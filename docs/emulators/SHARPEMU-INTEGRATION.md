# SharpEmu Environment

MetalSharp exposes SharpEmu as an **experimental PlayStation 5 research environment** in the Sharp Library. Most games do not run. Windows remains SharpEmu's primary development target, and macOS support is experimental.

MetalSharp and SharpEmu are unaffiliated with Sony. MetalSharp does not download, bundle, import, decrypt, patch, upload, or provide acquisition instructions for Sony firmware, games, keys, licenses, modules, fonts, updates, DLC, or decryption material.

## Managed layout

```text
~/.metalsharp/emulators/sharpemu/
├── current -> versions/<release-tag>
├── previous -> versions/<release-tag>
├── versions/<release-tag>/
│   ├── SharpEmu
│   ├── libMoltenVK.dylib
│   ├── libvulkan.1.dylib
│   ├── plugins/
│   ├── licenses/
│   ├── LICENSE.txt
│   ├── source-manifest.json
│   ├── activation-manifest.json
│   └── capabilities.json
├── home/
├── state/
│   ├── saves/
│   ├── custom-configs/
│   └── roots.json
├── cache/
│   ├── dotnet-bundle/
│   ├── ampr-index/
│   └── vulkan/
├── writable/
├── downloads/
├── staging/
├── sessions/
├── logs/
├── environment.json
├── update-policy.json
└── library-cache.json
```

Runtime versions are separate from mutable state. Activated version trees are read-only. Updates, rollback, repair, and runtime removal preserve saves, settings, roots, caches, logs, sessions, and external games.

## Host requirements

The current official macOS release is x86-64.

- Apple Silicon requires Rosetta 2.
- Intel Macs run x86-64 directly.
- The effective full-payload minimum is macOS 26 because bundled FFmpeg dylibs declare `minos 26.0`.
- `lsar` and `unar` are required.
- Installation requires at least 1 GiB of available transaction space.
- Vulkan uses the bundled MoltenVK; the experimental upstream native Metal backend is disabled.

Status reports architecture, macOS, Rosetta, archive tools, free disk, network containment, runtime integrity, and MoltenVK readiness separately.

## Secure stable installation

MetalSharp downloads only the exact official stable macOS x64 asset from `sharpemu/sharpemu` after user confirmation.

1. Fetch bounded GitHub release JSON over HTTPS.
2. Require a non-draft, non-prerelease stable `v...` tag.
3. Require exactly `sharpemu-<version>-osx-x64.tar.gz`.
4. Bind release ID, asset ID, tag, URL, name, size, digest, and timestamps.
5. Quarantine changed metadata for an already observed tag/asset.
6. Download to a new `.part` file with HTTPS-only redirects.
7. Verify exact bytes and SHA-256.
8. Preflight with `lsar`; reject traversal, links, devices, sparse entries, duplicates, case collisions, and bounds violations.
9. Extract with `unar` into same-volume staging.
10. Require the executable, Vulkan loaders, plugins, and license payload.
11. Inspect every Mach-O architecture, dependency, and deployment target.
12. Record pre-sign hashes in `source-manifest.json`.
13. Ad-hoc sign each native dependency and the main executable locally.
14. Verify signatures, MoltenVK loading, and the no-window nonexistent-eboot CLI probe.
15. Record post-sign hashes in `activation-manifest.json`.
16. Make the version tree read-only.
17. Atomically switch `current`, retaining `previous` for rollback.

The upstream macOS archive is not Developer ID signed or notarized. MetalSharp's local ad-hoc signature is not represented as upstream signing or Apple notarization.

## Update policy and rollback

Users can:

- check or refresh stable release metadata;
- install/update;
- pin the current version;
- unpin;
- skip the latest version;
- clear a skipped version;
- roll back to `previous`;
- remove managed runtime versions.

Download may occur while a game runs, but activation waits for all SharpEmu sessions to exit. Rollback and runtime removal are rejected while SharpEmu runs. Runtime removal is also rejected during an update transaction.

## Game discovery

Roots are selected with a native directory picker. MetalSharp rejects:

- symlinked and missing roots;
- `/`, system, library, applications, home, and MetalSharp-managed roots;
- duplicate or overlapping ancestor/descendant roots;
- more than 32 roots.

Scanning:

- searches for exact regular `eboot.bin` files;
- never follows symlinked directories;
- stops after depth 8, 20,000 entries, or 512 games;
- validates bounded ELF/fSELF leading structure;
- reads at most 1 MiB of `sce_sys/param.json` or adjacent `param.json`;
- reads title, PPSA title ID, content/master version, and localized title;
- validates local PNG artwork and never fetches PlayStation Store images;
- persists a launch index containing canonical path and executable size.

Launch reopens the indexed executable with no-follow semantics and compares current size to the saved scan identity. Replaced files fail with a “changed” error and require a rescan.

External roots are references. Removing a root changes only `state/roots.json` and `library-cache.json`; it never deletes external content.

## CLI-only launch

MetalSharp never launches SharpEmu's no-argument GUI or updater. It launches the exact active `SharpEmu` executable with reviewed arguments:

```text
--cpu-engine=native
--log-level=info
--log-file <isolated-log>
--window-mode=windowed
--scaling=fit
--vsync=on
<canonical-eboot.bin>
```

Fullscreen currently maps to SharpEmu's exclusive window mode but is not enabled by default in the UI.

The child environment redirects:

- `HOME`;
- .NET single-file extraction;
- saves;
- AMPR indexes;
- Vulkan pipeline cache;
- guest temporary/download/devlog/hostapp mounts;
- `TMPDIR`;
- SharpEmu logs.

MetalSharp removes inherited SharpEmu diagnostics, debugger/profiler, dynamic-loader, proxy, RenderDoc, native-Metal, writable-app0, and network-redirection variables before launch.

## Guest networking

SharpEmu guest networking can create real host sockets.

Default behavior:

- MetalSharp runs SharpEmu through `sandbox-exec` with all network operations denied.
- Host readiness verifies that the sandbox starts and cannot connect to a MetalSharp-owned loopback listener.
- If containment is unavailable, a default launch fails closed.

Explicit opt-in:

- The Sharp Library has an “Allow unrestricted guest networking” checkbox.
- Enabling it shows a persistent danger state.
- Every network-enabled launch requires a second confirmation.
- The launch runs without the network-denial profile.
- The session record stores `networkEnabled: true`.

No mode uploads MetalSharp telemetry, diagnostics, game metadata, or compatibility reports automatically.

## Process supervision

Every launch receives:

- a stable session ID and game ID;
- PID and process group;
- exact executable and runtime tag;
- canonical game path;
- MetalSharp log path;
- start timestamp;
- network-policy value.

MetalSharp waits until the child has executed the exact managed executable before reporting success. Backend restart recovery validates command path and process start time before accepting a PID.

Stop behavior:

1. SIGINT to the validated process group;
2. bounded graceful wait;
3. SIGTERM;
4. final SIGKILL fallback.

Exit code/signal and the latest log remain visible on the game card. Logs remain local and may contain title IDs, game paths, module names, and crash details.

## Backend API

Read endpoints:

```text
GET /emulators
GET /sharp-library/sharpemu/status
GET /sharp-library/sharpemu/games
GET /sharp-library/sharpemu/cover?id=<stable-id>
GET /sharp-library/sharpemu/sessions
GET /sharp-library/sharpemu/update/check
GET /sharp-library/sharpemu/update/progress
```

Mutation endpoints:

```text
POST /sharp-library/sharpemu/scan
POST /sharp-library/sharpemu/add-root
POST /sharp-library/sharpemu/remove-root
POST /sharp-library/sharpemu/launch
POST /sharp-library/sharpemu/stop
POST /sharp-library/sharpemu/update/refresh
POST /sharp-library/sharpemu/update/install
POST /sharp-library/sharpemu/update/rollback
POST /sharp-library/sharpemu/pin-current
POST /sharp-library/sharpemu/unpin
POST /sharp-library/sharpemu/skip-update
POST /sharp-library/sharpemu/clear-skip
POST /sharp-library/sharpemu/remove-runtime
```

There are no firmware, key, module, package, decryption, debugger, upstream-GUI, or compatibility-submission endpoints. Request objects reject unknown fields and wrong primitive types.

## Electron boundary

The preload exposes only:

- bounded backend requests;
- a SharpEmu game-root picker;
- path reveal restricted to the SharpEmu environment and registered roots;
- exact official FAQ and compatibility URLs.

The renderer cannot open arbitrary SharpEmu paths or URLs. A per-game URL is created only from a validated `PPSA` plus five digits.

## Testing and evidence

`app/src-c/tests/sharpemu_update_test.py` covers:

- provider/status/update contracts;
- synthetic real Mach-O transaction installation;
- archive digest and path safety;
- local signing and read-only activation;
- source and activation manifests;
- discovery, metadata, artwork, and PPSA parsing;
- symlinked-root rejection;
- request schema rejection;
- denied-network and explicit-network session records;
- process supervision, sessions, stop, and runtime-removal blocking;
- changed launch target rejection;
- replaced artwork symlink rejection;
- mutable upstream asset quarantine;
- state and external-game preservation;
- malicious symlink archive rejection.

The test-only probe bypass works only when:

- the backend path is under `src-c/build/` or `src-c/build-asan/`;
- both release and download fixtures are present;
- the explicit test variable is set.

Packaged binaries reject that bypass.

See [SHARPEMU-UPSTREAM-CONTRACT.md](SHARPEMU-UPSTREAM-CONTRACT.md) for frozen upstream evidence and [SHARPEMU-INTEGRATION-ROADMAP.md](SHARPEMU-INTEGRATION-ROADMAP.md) for phase acceptance.
