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
- Phase 4 runs the interactive VC++ 2015-2022 x64 and x86 installers only
  through `runtime/wine/bin/metalsharp-wine` and only after both the canonical
  complete-runtime gate and the Steam all-architecture prefix gate pass. Both
  installer commands pin `WINEBUILDDIR`, the Steam prefix, and all FEX TSO
  modes off; they may wait up to 45 minutes for user interaction.
- Redistributables download from Microsoft's current aka.ms endpoints using
  `/usr/bin/curl`, require a valid PE bootstrap and architecture-appropriate
  payload size, and verify x64 DLLs in `system32` or x86 DLLs in `syswow64`.
  Microsoft's x64 redistributable intentionally uses an i386 PE bootstrap, so
  do not misclassify its bootstrap machine as the installed payload target.
- Phase 5 does not change the Launch Steam endpoint, UI button, executable, or
  flags. Its wrapper maintenance now prefers the canonical complete-runtime
  asset at `runtime/integration/steam-webhelper/steamwebhelper.exe`, whose
  accepted SHA-256 is
  `f46a1e8c39c850ba22861f63559f13b4f68557acf04a92e6d1b899769b2ea1f9`.
- Steam updates are repaired before launch only when the wrapper/real-helper
  pair fails its hash-and-size contract. Wrapper deployment is refused unless
  the target is exactly the MetalSharp Steam prefix or a MetalSharp-managed
  bottle prefix; never deploy into CrossOver, another Wine installation, or an
  arbitrary path.
- Phase 6 makes M12 a vkd3d-proton route, not a DXMT route. Its only PE
  owners are `lib/vkd3d-proton/x86_64/{d3d12.dll,d3d12core.dll}` and
  `lib/dxvk/x86_64/dxgi.dll`; the host handoff is Wine Vulkan to the bundled
  ARM64 MoltenVK library. Never restore winemetal, `dxgi_dxmt`, or
  `lib/dxmt_m12` to M12.
- M12 bottle save migrates old component ids to `m12_vkd3d_d3d12`,
  `m12_vkd3d_d3d12core`, `m12_dxvk_dxgi`, `m12_winevulkan`, and
  `m12_moltenvk`. Save, prepare, Play, dry-run, runtime doctor, migration, and
  the complete-runtime gate all validate the same ownership boundaries.
- M9/M10/M10(32)/M11/M11(32) consume the complete runtime directly, not the
  retired split-graphics manifest gate. M10/M11 use DXMT's
  `aarch64-windows` PE lane; M10(32)/M11(32) use its `i386-windows` PE lane;
  both cross into the native ARM64 `aarch64-unix/winemetal.so`. M9 uses the
  Wine build's x86_64/i386 D3D9 lanes. D3DMetal remains a separate
  Homebrew-owned GPTK prefix and must not borrow M12 runtime DLLs.
- The M12 launch environment contains vkd3d-proton/DXVK cache paths and no
  DXMT/winemetal variables. All complete-runtime launch adapters continue to
  pin every FEX TSO mode off.
- Phase 6 validation: the canonical archive passed the strengthened
  prepare-only layout gate; extracted M12 PE DLLs identify as x86-64, the Wine
  Vulkan Unix bridge identifies as ARM64, DXMT's i386 PE lane identifies as
  i386, its `aarch64-windows` PE lane identifies as x86-64/ARM64EC-compatible,
  and its Unix winemetal bridge identifies as ARM64. The deterministic Rust
  suite passes 651/651, strict Clippy passes, all Rust targets build, and the
  TypeScript/Vite production build passes.
- Phase 7 adds global Controller `[D | X]` and Msync controls to the left
  sidebar. Settings persist as `controllerMode` (`dinput` or `xinput`) and
  `msyncEnabled`; legacy snake-case aliases are read and written as well.
- Every managed Wine path receives `METALSHARP_CONTROLLER_MODE` and
  `WINEMSYNC`, including MTSP direct routes, Steam route handoffs, Steam
  setup/update processes, ordinary Wine launches, prefix initialization, and
  GOG. Existing installs default to XInput and Msync enabled. Environment
  overrides remain available as `METALSHARP_CONTROLLER_MODE` and
  `METALSHARP_MSYNC`.
- Phase 7 validation: the deterministic Rust suite passes 653/653, strict
  Clippy passes, all Rust targets build, and the TypeScript/Vite production
  build passes.
- Phase 8 removes every retired split-runtime archive from the DMG resource
  list and release workflow. `tools/dmg/prepare-complete-runtime-assets.sh`
  downloads and pins the v0.60.0 installer, manifest, public source archive,
  four runtime parts, checksum files, and reassembly instructions.
- The DMG embeds the installer, manifest, public source/provenance payload,
  part checksums, and reassembly instructions under `runtime-bundle/`; it does
  not embed the four runtime parts. Release CI publishes those parts as
  separate assets so every file stays below GitHub's 2 GiB release-asset cap.
  The backend prefers the packaged installer and passes its directory through
  `--bundle-dir`, allowing local discovery before network download.
- The release gate verifies every asset hash against the pinned manifest,
  verifies each part against `PARTS-SHA256SUMS.txt`, streams all four parts
  through the canonical reconstructed SHA-256, syntax-checks the installer,
  and rejects any DMG that still contains retired split archives.
- `tools/ci/verify-dmg-workflow.py` enforces the same six packaged assets and
  ten separately published release assets. It must reject any return of the
  eight split archives or the retired developer-SDK release job.
- Phase 8 validation: package-only assets from the live dependency release
  passed hash and shell verification; release YAML parses; package metadata
  contains only the complete-runtime resource contract; the deterministic
  Rust suite passes 653/653; strict Clippy, all Rust targets, and the
  TypeScript/Vite production build pass.
- The post-Phase-8 GOG pass adds
  `MetalSharp-GOG-Support-arm64-1.2.2.tar.zst` to the dependency release and
  DMG bootstrap payload. It is a self-contained thin ARM64 Heroic GOGDL 1.2.2
  executable with Requests/TLS, the compiled xdelta3 module, licenses,
  per-file hashes, and pinned provenance. The archive SHA-256 is
  `f13075f27d5155e84199619410936931b32310c4ec4161de992c1f727ac24155`.
- The complete-runtime installer validates and stages GOG support under
  `runtime/integration/gog/`. Existing accepted Wine runtimes receive this
  small layer in place without reassembling, extracting, or replacing the
  multi-gigabyte Wine runtime. Runtime readiness now requires the GOG archive
  marker and executable.
- Steam and GOG prefix creation share `runtime_prefix.rs`. Both stage ARM64
  WoW64 providers, all i386 builtins, canonical ARM64/ARM64EC execution
  providers, and GStreamer before explicit ARM64 wineboot; wait for the exact
  prefix wineserver; restage and verify providers; and require the same
  ARM64/ARM64EC/x86_64/i386 acceptance gate. A `drive_c` directory alone is
  never GOG-prefix acceptance.
- GOG prefers the canonical bundled native ARM64 GOGDL path over legacy
  `~/.metalsharp/tools/gogdl`. Download, import, and Play commands inherit the
  complete runtime's `WINEBUILDDIR`, library paths, controller/Msync settings,
  and all three FEX TSO-off flags. The source-install fallback is pinned to
  Heroic GOGDL `v1.2.2`, never an unpinned main branch.
- GOG validation: the native bundle passed archive/per-file hashes, Mach-O,
  code-signature, version, embedded-xdelta, and unauthenticated command probes;
  installer add/repair and idempotency completed in two seconds each against
  a disposable existing-runtime fixture; the deterministic Rust suite passes
  655/655; strict Clippy, all Rust targets, TypeScript, Vite, DMG workflow, and
  shell gates pass.
- Wine Mono 11.2.0 is part of the complete runtime. The GOG and Settings tabs
  no longer offer a second interactive MSI download/install/reset path, and
  the retired `/wine-mono/*` backend endpoints and installer module are gone.
  This does not remove bundled Mono, the Mono/FNA launch lanes, or bottle-level
  compatibility metadata. Post-removal validation passes 644/644 Rust tests,
  strict Clippy, all Rust targets, and the Electron production build.

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
| `M12` | vkd3d-proton D3D12 + DXVK DXGI through Wine Vulkan and ARM64 MoltenVK | Peak, Silksong, Elden Ring, D3D12 investigation titles |
| `D3DMetal` | Homebrew GPTK/D3DMetal in its own prefix | Explicit GPTK route |
| `Mono/FNA` | Windows XNA/FNA through native Mono, staged FNA/XNA assemblies, and host shims | Celeste, Terraria |

Internal route ids such as `dxmt`, `wine_bare`, `m32`, `steam`, `macos_steam`, and `m13` stay backend-parseable for legacy records, diagnostics, and fallback behavior, but they are not normal bottle route buttons.

### Key Paths at Runtime

- Wine runtime: `~/.metalsharp/runtime/wine/`
- Wine prefix: `~/.metalsharp/prefix-steam/`
- DXMT M10/M11 PE DLLs: `~/.metalsharp/runtime/wine/build-ec/dxmt-v0.80/aarch64-windows/`
- DXMT M10(32)/M11(32) PE DLLs: `~/.metalsharp/runtime/wine/build-ec/dxmt-v0.80/i386-windows/`
- DXMT native host bridge: `~/.metalsharp/runtime/wine/build-ec/dxmt-v0.80/aarch64-unix/winemetal.so`
- M9 Wine D3D9 DLLs: `~/.metalsharp/runtime/wine/build-ec/dlls/d3d9/{x86_64,i386}-windows/`
- M12 vkd3d-proton DLLs: `~/.metalsharp/runtime/wine/lib/vkd3d-proton/x86_64/`
- M12 DXVK DXGI: `~/.metalsharp/runtime/wine/lib/dxvk/x86_64/dxgi.dll`
- M12 Wine Vulkan bridge: `~/.metalsharp/runtime/wine/build-ec/dlls/winevulkan/`
- M12 MoltenVK host library: `~/.metalsharp/runtime/wine/build-ec/dlls/win32u/libMoltenVK.dylib`
- DXVK i386 DLLs: `~/.metalsharp/runtime/wine/lib/dxvk/i386/`
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
