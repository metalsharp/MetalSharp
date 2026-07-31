# AGENTS.md
**Updated:** 2026-07-31

Guide for AI agents working on the MetalSharp repository.

## 0.60.0 Preview Release Work

Development is isolated on `agent/0.60-preview-release`. The saved phase plan is
`~/Documents/obsidian/Avery'sVault/0.60.0 Official Preview Release Plan.md`.

- Phase 1 replaces first-run split-bundle installation with
  `scripts/install-metalsharp-wine-runtime.sh` from release
  `v0.60.0-dependency-bundles`.
- The canonical complete-runtime SHA-256 is
  `93a456a40a7bf0ad2fecace5c01c58a366f85cc2901f6f8780c056c9e3b256ee`.
- Runtime completion is gated by `.metalsharp-runtime-install` plus Wine,
  wineserver, fonts/NLS, DXVK i386, vkd3d-proton D3D12, OpenGL-Metal, and
  MoltenVK payloads. Do not restore the old six-archive setup gate.
- The runtime installs transactionally at `~/.metalsharp/runtime`; prefixes,
  Steam, bottles, saves, and shader caches remain outside that directory.
- Phase 1 keeps Rosetta installation until macOS 28 and keeps GPTK/Homebrew as
  a separate route. The complete runtime launch adapters explicitly disable
  FEX TSO, vector TSO, and memcpy/set TSO.
- Phase 2 creates `~/.metalsharp/prefix-steam` as a single all-architecture
  prefix. Before wineboot it stages ARM64 WoW64 providers, every i386 Wine
  builtin, and the accepted ARM64/ARM64EC CPU providers; after wineboot it
  restages/verifies providers and requires both system32 and syswow64 gates.
- Steam installation enables the ntdll host notification only for installer
  processes. `POST /steam/handoff` accepts exactly two serialized callbacks:
  cycle 1 shuts down only the Steam prefix and relaunches its updater; cycle 2
  shuts it down, deploys and verifies the CEF wrapper, and writes
  `.metalsharp-steam-install-complete`. Steam is not reported installed before
  that marker exists. All install and handoff launches keep every FEX TSO mode
  disabled.
- Phase 3 makes a functioning Homebrew installation a hard setup gate. Both
  Electron and the Rust backend require `brew --version` to succeed; file
  presence alone is not acceptance. The Terminal installer downloads the
  official Homebrew installer with curl, validates it with `bash -n`, runs it,
  then verifies the installed brew before reporting success.
- GPTK/D3DMetal remains separate from the complete MetalSharp Wine runtime and
  is owned by Homebrew. Saving a D3DMetal bottle invokes the existing
  `gcenx/wine/game-porting-toolkit` tap/trust/cask route; do not stage a private
  GPTK copy into the main runtime.

## What This Project Is

MetalSharp is a macOS app that runs Windows Steam games and Windows programs via Wine + Metal translation. It's an Electron app with a Rust HTTP backend, a C++ native D3D/Metal engine, per-game engine routing, runtime bottles, installer profiles, and Linux `.deb` packaging.

## Repository Structure

```
app/
├── src-rust/                    Rust HTTP backend (tiny_http server on port 9274)
│   └── src/
│       ├── main.rs              HTTP router — all /launch, /steam/*, /setup/*, /config, /logs endpoints
│       ├── bottles.rs           Runtime bottles, installer profiles, runtime doctor, redist/source checks
│       ├── launch.rs            Engine detection + game launch — the core routing logic
│       ├── steam.rs             Steam process management, library, install/uninstall, CEF wrapper
│       ├── setup.rs             Per-game preparation (shim builds, DLL staging, FNA runtime)
│       ├── installer.rs         Dependency installer (Wine bundle, Rosetta, Xcode CLI, GPTK, Mono)
│       ├── migrate.rs           Runtime migration + preservation of Steam/prefix/game/bottle state
│       ├── scan.rs              Game library scanner (Steam appmanifest parsing, wine path resolution)
│       ├── sharp_library.rs     Sharp Library imports, installer launch, bottle app imports
│       └── updater.rs           Self-update via GitHub releases DMG download
├── src/
│   ├── main/                    Electron main process
│   └── renderer/                Electron renderer (UI, library, setup wizard)
├── bundles/                     Pre-packaged deps (metalsharp_bundle.tar.zst, SteamSetup.exe, etc.)
├── updater/                     Python update runtime script
├── package.json                 Electron app manifest
└── src-rust/Cargo.toml          Rust backend manifest

src/                             C++ native D3D11/D3D12/DXGI/XAudio2/XInput → Metal implementations
├── d3d/d3d11/                   D3D11 device, context, shaders, resources
├── d3d/d3d12/                   D3D12 device, command queue, command list, resources
├── dxgi/                        DXGI factory, adapter, swap chain
├── metal/                       Metal device, command queue, pipeline, shader translation
├── audio/                       XAudio2 → CoreAudio backend
├── input/                       XInput → GameController backend
├── perf/                        Shader cache, pipeline cache, MetalFX upscaler, GPU profiler
├── runtime/                     PE hooks, compat database, crash diagnostics, DRM detector
├── loader/                      Native PE loader + Win32 shims (kernel32, user32, etc.)
├── wine/                        Wine-specific integration code
├── steam/                       Steam integration
├── win32/                       Win32 API shims (kernel32, user32, registry, etc.)
└── fna/                         FNA/XNA game support (Terraria, Celeste shims)

include/                         C++ public headers
tests/                           C++ test suite (20+ tests: D3D11, D3D12, DXBC, Metal, audio, input)
tools/
├── launcher/                    Native launcher (metalsharp binary + Wine prefix management)
├── dmg/                         DMG packaging scripts
└── linux/                       DEB/Docker/runtime tarball/GHCR package scripts
scripts/
└── install-gptk-runtime.sh      Homebrew GPTK runtime install
configs/                         MTSP rules + Mono DLL maps for FNA games
docs/                            Architecture docs + game compatibility matrix
CMakeLists.txt                   C++ build (native engine + tests)
install.sh                       CLI build script (cmake + test runner)
```

## Key Concepts

### MTSP Routing and Runtime Bottles

Modern runtime paths use MTSP pipeline ids and bottle profiles. Steam games get `steam_<appid>` bottles that are launch-authoritative for runtime checks. Wine Steam remains the live background Steam client; env-dependent Steam routes launch the game executable directly with the bottle prefix, route env, and Steam identity variables instead of trying to make an already-running Steam process inherit new env.

| Public route | Method | Example games |
|--------|--------|--------------|
| `M9` | D3D9 / 32-bit capable DXMT-family route | Nidhogg 2, Undertale, Blasphemous, Dave the Diver |
| `M10` | D3D10 to Metal | D3D10 apps/games |
| `M11` | D3D11 to Metal | Rain World, Schedule I, Subnautica BZ |
| `M12` | D3D12 to Metal through the isolated `dxmt-m12` runtime surface | Peak, Silksong, Elden Ring, D3D12 investigation titles |
| `Mono/FNA` | Windows XNA/FNA through native Mono, staged FNA/XNA assemblies, and host shims | Celeste, Terraria |

Internal route ids such as `dxmt`, `wine_bare`, `m32`, `steam`, `macos_steam`, and `m13` stay backend-parseable for legacy records, diagnostics, and fallback behavior, but they are not normal bottle route buttons.

### Key Paths at Runtime

- Wine runtime: `~/.metalsharp/runtime/wine/`
- Wine prefix: `~/.metalsharp/prefix-steam/`
- DXMT PE DLLs for M9/M10/M11: `~/.metalsharp/runtime/wine/lib/dxmt/x86_64-windows/`
- DXMT M12 PE DLLs: `~/.metalsharp/runtime/wine/lib/dxmt-m12/x86_64-windows/`
- DXMT M12 Unix bridge and sidecars: `~/.metalsharp/runtime/wine/lib/dxmt-m12/x86_64-unix/`
- DXVK i386 DLLs: `~/.metalsharp/runtime/wine/lib/dxvk/i386-windows/`
- MoltenVK ICD: `~/.metalsharp/runtime/wine/etc/vulkan/icd.d/MoltenVK_icd.json`
- DXMT config: `~/.metalsharp/runtime/wine/etc/dxmt.conf`
- Local redistributables: `~/.metalsharp/runtime/redist/`
- Runtime bottles: `~/.metalsharp/bottles/<bottle_id>/`
- Bottle prefix: `~/.metalsharp/bottles/<bottle_id>/prefix/`
- Bottle manifest: `~/.metalsharp/bottles/<bottle_id>/bottle.json`
- Bottle logs: `~/.metalsharp/bottles/<bottle_id>/logs/`
- Shader cache: `~/.metalsharp/shader-cache/<engine>/<appid>/`
- Game local copies: `~/.metalsharp/games/<appid>/`
- Logs: `~/.metalsharp/logs/`

### HTTP API (main.rs)

Backend listens on `127.0.0.1:9274` (override with `METALSHARP_PORT`). Key endpoints:

- `POST /game/launch-auto` — launch game by appid with engine auto-detection
- `POST /game/prepare` — prepare game runtime (shims, DLLs, config)
- `GET /steam/library` — full game library (owned + installed)
- `POST /steam/launch` — start Wine Steam
- `POST /steam/stop` — kill Wine Steam, wineserver, Wine Steam service helpers, detached Wine desktop/tray helpers, and headless Wine console hosts
- `POST /steam/launch-game` — Steam game launch with bottle preflight; env-dependent routes keep Wine Steam alive and spawn the game through the selected MTSP pipeline
- `POST /steam/runtime-doctor` — inspect a Steam game's bottle/runtime readiness
- `GET /sharp-library` — Sharp Library apps and imported Windows programs
- `POST /sharp-library/install` — Install Windows Program flow for EXE/MSI/installer bottles
- `POST /sharp-library/import-bottle-app` — import detected app candidates from a bottle
- `GET /bottles` — list runtime bottles
- `GET /bottles/profiles` — list supported bottle runtime profiles
- `GET /bottles/compatibility-matrix` — bottle compatibility cases
- `GET /bottles/redist-sources` — local redistributable source status
- `POST /bottles/doctor` — diagnose bottle readiness
- `POST /bottles/prepare` — prepare runtime assets/components for a bottle
- `POST /bottles/repair-component` — repair missing runtime components
- `POST /bottles/set-runtime-profile` — change a bottle profile
- `POST /bottles/set-windows-version` — apply a Wine Windows-version mode
- `POST /bottles/relaunch-installer` — relaunch an installer bottle's source installer
- `GET /setup/dependencies` — check which deps are installed
- `POST /setup/install-all` — run full dependency installer
- `POST /kill` — kill game by pid or appid
- `GET /logs` — recent log entries
- `GET /logs/stream` — streaming logs
- `GET /logs/crash-reports` — discovered crash report metadata

## Build Commands

### Rust backend
```bash
cd app/src-rust && cargo build --release
```

### Electron app
```bash
cd app && npm install && npm run build
```

### Everything (Rust + TypeScript)
```bash
cd app && npm run build:all
```

### C++ native engine + tests
```bash
./install.sh
# or manually:
mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build . --parallel $(sysctl -n hw.ncpu)
ctest --output-on-failure
```

### DMG packaging
```bash
./tools/dmg/create-bundles.sh
cd app && npx electron-builder --mac dmg --arm64
```

### Linux DEB and package assets
```bash
cd app && npm run deb
cd app && npm run deb:docker
tools/linux/create-release-tarballs.sh
tools/linux/publish-oci-packages.sh
```

## CI Workflows

Current CI is split between PR smoke coverage, lightweight main-push workflow validation, and tag-driven release packaging:

| Workflow | Triggers | What it does |
|----------|----------|-------------|
| `pr-ci.yml` | PRs to `main` | Shell CI, Metal CI, Vue CI, Rust CI, Electron CI, C/C++/Obj-C CI, and lightweight `DMG Workflow CI` contract validation |
| `ci.yml` | pushes to `main` | Main-branch smoke coverage plus `DMG Workflow CI` contract validation; it does not publish release artifacts |
| `release.yml` | tags `v*` | Developer SDK publish, full arm64 DMG build, DMG runtime-asset verification, release artifact upload, and package publication |
| `publish-linux-packages.yml` | manual | Re-publish Linux DEB/runtime release assets to GHCR with ORAS |

## Version Bumping

Five files must be updated together for a version bump:

| File | Field |
|------|-------|
| `app/package.json` | `"version": "X.Y.Z"` |
| `app/package-lock.json` | root/package lock `"version": "X.Y.Z"` |
| `app/src-rust/Cargo.toml` | `version = "X.Y.Z"` |
| `app/src-rust/Cargo.lock` | `metalsharp-backend` package `version = "X.Y.Z"` |
| `CMakeLists.txt` | `project(metalsharp VERSION X.Y.Z ...)` |

The Rust backend reads its version from `CARGO_PKG_VERSION` (set by Cargo.toml). The CI release workflow (`ci.yml`) reads the version from the git tag — if the tag version differs from `package.json`, it rewrites `package.json` before building.

### Tag and release process

```bash
# 1. Update all synchronized version files to the new version
# 2. Commit and push to main
# 3. Create and push the tag
git tag v<X.Y.Z>
git push origin v<X.Y.Z>
# 4. CI builds the DMG and creates a GitHub Release automatically
```

The `ci.yml` release job only triggers on tag pushes matching `v*`. It builds the full app, packages the DMG and DEB, creates Linux runtime tarballs, publishes Linux OCI package assets, and uploads release artifacts with auto-generated release notes.

The updater module (`updater.rs`) checks for new releases by hitting `https://api.github.com/repos/aaf2tbz/metalsharp/releases/latest` and comparing the tag to `CARGO_PKG_VERSION`.

## Suggested Tests Before Committing

> **Canonical gates:** see [`docs/optimization-roadmap/local-gates.md`](docs/optimization-roadmap/local-gates.md)
> for the full local gate set, including the D3D12 Metal SDK probes CI cannot
> run and the Phase 1–8 backend diagnostic routes.

### Rust changes
```bash
cd app/src-rust
cargo fmt --all -- --check     # must pass (CI enforces)
cargo clippy --all-targets -- -D warnings  # must pass (CI enforces)
cargo test                     # unit tests
cargo build --release          # verify build
```

### C++ changes
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build . --parallel $(sysctl -n hw.ncpu)
ctest --output-on-failure
```

### TypeScript/Electron changes
```bash
cd app
npm run build                  # tsc + copy assets
npx biome check src/           # lint (CI enforces)
```

### Integration sanity
- `POST /setup/install-all` → verify progress endpoint shows completion
- `GET /steam/status` → verify returns valid JSON
- `POST /game/launch-auto` with a known appid → verify correct engine selected (check logs)
- `GET /steam/library` → verify games appear with correct launch_method
- `POST /steam/runtime-doctor` with a numeric appid → verify bottle checks/components are returned
- `GET /bottles` and `GET /bottles/redist-sources` → verify bottle metadata loads

## Common Pitfalls

- **Don't edit the Wine plist before running `daemon start`** — let signet create it first, then patch HOME and kickstart
- **DXVK i386 DLLs are at `lib/dxvk/i386-windows/`**, NOT `lib/wine/i386-windows/` — the latter is for Wine builtins
- **Shader cache is per-appid** (`~/.metalsharp/shader-cache/<engine>/<appid>/`), not per-exename
- **Celeste (504230) and Terraria (105600) are Mono/FNA** — they use the native Mono/XNA/FNA lane with Steamworks/audio/native-library shims.
- **Goat Simulator (265930) is currently an M9/D3D9 investigation target** — it still needs native .NET 4.0 CLR work before it can be promoted.
- **CMakeLists.txt version must match Cargo.toml and package.json** — all three are independently read
- **app/package-lock.json version must match package.json** — npm package metadata and release automation both see it
- **Steam game bottles are launch-authoritative, but Steam stays alive as the client** — bottles preflight and bind runtime assets; env-dependent routes spawn the game process with `SteamAppId`/`SteamGameId` while Wine Steam remains connected
- **Installer bottles use their own prefixes** — apps imported from installer bottles must keep `bottle_id` so Sharp Library launches them from that bottle
- **Linux Docker DEB builds can leave `dist/` root-owned** — `tools/linux/create-release-tarballs.sh` repairs ownership before writing `dist/packages`
- **`winemetal.so` has no i386-unix version** — it uses WoW64 thunks for 32-bit PE clients, always lives in x86_64-unix/
- **Steam auto-updates overwrite the steamwebhelper wrapper** — `deploy_steamwebhelper_wrapper()` handles this but it's a known pain point
