# AGENTS.md

**Updated:** 2026-10-05

Guide for AI agents working on MetalSharp.

## Project

MetalSharp is a native macOS app (Rust/GPUI, in `app-gpui/`) that runs Windows games and programs through Wine and Metal translation. The application uses a C HTTP backend, a C/C++/Objective-C native graphics engine, runtime bottles, per-game MTSP routing, and packaged runtime assets.

## Repository Structure

```text
app/
├── src-c/                      C HTTP backend (127.0.0.1:9274)
│   ├── include/                Backend public headers
│   ├── runtime/                Routes, launch/runtime orchestration, providers, setup, migration
│   ├── tests/                  C unit, transaction, and HTTP smoke tests
│   └── Makefile                Backend build and test entry point
├── native/                     Native helpers packaged into Resources/scripts/tools/native
├── tools/                      Bundled CLI tools (zstd, unar, wrestool, ...)
├── bundles/                    Packaged runtime assets
├── updater/                    Updater scripts (update.sh)
└── tests/                      Node source-contract tests for the backend

app-gpui/                       GPUI desktop app: UI, backend supervision, packaging

src/                            Native D3D/Metal, audio, input, runtime, Wine, and FNA code
include/                        Native public headers
tests/                          Native C/C++ tests
tools/                          Packaging, runtime, CI, and D3D12 SDK tooling
configs/                        Route rules and runtime configuration
docs/                           Architecture, runtime, emulator, and compatibility docs
CMakeLists.txt                  Native engine build
```

The C backend is the only backend. The GPUI app's packaging builds it from `app/src-c` and packages `app/src-c/build/metalsharp-backend` as `Contents/Resources/runtime/metalsharp-backend`; the app supervises it on port 9274.

## Runtime and Routing

Steam games use `steam_<appid>` bottles. Wine Steam stays the background client; game launches run through the selected route with the shared prefix, route env, cache paths, and Steam identity variables.

Routes (D3DMetal, DXMT, DXMT(32), D3D9, VKD3D, Mono/FNA) and their internal lane names (`m9`–`m12`) are documented in [Architecture](docs/architecture.md).

Important runtime paths:

- Home: `~/.metalsharp/`
- Wine: `~/.metalsharp/runtime/wine/`
- Steam prefix: `~/.metalsharp/prefix-steam/`
- Ubisoft Connect prefix: `~/.metalsharp/prefix-ubisoft/`
- Bottles: `~/.metalsharp/bottles/<bottle_id>/`
- Logs: `~/.metalsharp/logs/`
- Shader cache: `~/.metalsharp/shader-cache/<pipeline>/<appid>/`

## Backend API

The backend listens on `127.0.0.1:9274`; `METALSHARP_PORT` is validation-only. The router is `app/src-c/runtime/backend.c`. Domain implementations live beside it, including `steam_actions.c`, `bottles.c`, `setup.c`, `migration.c`, `epic.c`, `gog.c`, `gamejolt.c`, `pcsx2.c`, `rpcs3.c`, `shadps4.c`, and `sharpemu.c`.

Representative endpoints:

- `GET /status`
- `GET /steam/library`
- `GET /ubisoft/library`
- `POST /ubisoft/launch` / `POST /ubisoft/stop`
- `POST /steam/launch-game`
- `POST /steam/stop`
- `GET /bottles`
- `POST /bottles/doctor`
- `POST /bottles/prepare`
- `GET /sharp-library`
- `GET /setup/dependencies`
- `POST /setup/install-all`
- `GET /logs`
- `POST /kill`

## Build and Test

### C backend

```bash
make -C app/src-c test
```

### GPUI app

```bash
cd app-gpui
cargo fmt --check && cargo test
ACTOOL=/path/to/Xcode.app/Contents/Developer/usr/bin/actool ./package-app.sh
```

`package-app.sh` builds the C backend and the release GPUI app into `app-gpui/target/MetalSharp.app` (bundle id `com.metalsharp.app`). Release builds need a `CARGO_TARGET_DIR` on the internal disk. See [Development](docs/development.md).

### Native engine

```bash
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build-native --parallel "$(sysctl -n hw.ncpu)"
ctest --test-dir build-native --output-on-failure
```

### DMG

```bash
tools/dmg/create-bundles.sh
tools/dmg/build-dmg.sh   # packages the GPUI app and builds MetalSharp-<version>-arm64.dmg
```

Do not rebuild the runtime archive unless explicitly requested.

## CI

- `pr-ci.yml`: shell, rules, docs, Metal, C backend, native, and DMG workflow checks.
- `gpui-preview.yml`: GPUI app format, tests, route inventory, and backend smoke tests.
- `ci.yml`: equivalent main-branch validation.
- `release.yml`: native build, C backend and GPUI app packaging, DMG verification, signing, and publication.

## Versioning

Keep these synchronized:

- `CMakeLists.txt` (packaging reads the app version from here)
- `app/src-c/Makefile`

Use `tools/release/set-version.sh X.Y.Z`.

## Required Practices

- The packaged C backend is authoritative.
- Use port 9274 for normal operation; temporary ports are validation-only.
- Package and atomically install backend/frontend changes before final validation.
- Do not preview the GPUI app against a temporary `METALSHARP_HOME` except with `--connected-validation`.
- Preserve firmware, saves, configuration, profiles, caches, and external games during emulator updates.
- Never log or persist launcher secrets.
- Never use AppleScript to quit a possibly closed MetalSharp app; addressing it launches it first.
- Leave `/Applications/MetalSharp.app` closed after final validation unless the user requests otherwise.
- Validate C formatting, C tests, Rust formatting/tests, packaging, hashes, and deep code signing for release-facing changes.
