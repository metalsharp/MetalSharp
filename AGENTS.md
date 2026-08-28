# AGENTS.md

**Updated:** 2026-08-28

Guide for AI agents working on MetalSharp.

## Project

MetalSharp is a macOS Electron app that runs Windows games and programs through Wine and Metal translation. The application uses a C HTTP backend, a C/C++/Objective-C native graphics engine, runtime bottles, per-game MTSP routing, and packaged runtime assets.

## Repository Structure

```text
app/
├── src-c/                      C HTTP backend (127.0.0.1:9274)
│   ├── include/                Backend public headers
│   ├── runtime/                Routes, launch/runtime orchestration, providers, setup, migration
│   ├── tests/                  C unit, transaction, and HTTP smoke tests
│   └── Makefile                Backend build and test entry point
├── src/main/                   Electron main process and backend supervision
├── src/renderer/               Vue renderer, library, settings, and setup UI
├── bundles/                    Packaged runtime assets
├── updater/                    Updater scripts
└── package.json                Electron build and packaging configuration

src/                            Native D3D/Metal, audio, input, runtime, Wine, and FNA code
include/                        Native public headers
tests/                          Native C/C++ tests
tools/                          Packaging, runtime, CI, and D3D12 SDK tooling
configs/                        MTSP rules and runtime configuration
docs/                           Architecture, runtime, emulator, and compatibility docs
CMakeLists.txt                  Native engine build
```

The C backend is the only backend. Electron builds it from `app/src-c` and packages `app/src-c/build/metalsharp-backend` as `Contents/Resources/runtime/metalsharp-backend`.

## Runtime and Routing

Steam games use `steam_<appid>` bottles. Wine Steam remains the background client; routes that need per-game environment variables spawn the game through the selected MTSP pipeline.

| Route | Purpose |
|---|---|
| `M9` | D3D9 and compatible 32-bit titles |
| `M10` / `M10(32)` | D3D10 translation |
| `M11` / `M11(32)` | D3D11 translation |
| `M12` | D3D12 through the isolated DXMT M12 runtime |
| `VKD3D` | D3D12 through vkd3d-proton and MoltenVK |
| `D3DMetal` | Apple Game Porting Toolkit runtime |
| `Mono/FNA` | XNA/FNA through Mono and native shims |

Important runtime paths:

- Home: `~/.metalsharp/`
- Wine: `~/.metalsharp/runtime/wine/`
- Steam prefix: `~/.metalsharp/prefix-steam/`
- Bottles: `~/.metalsharp/bottles/<bottle_id>/`
- Logs: `~/.metalsharp/logs/`
- Shader cache: `~/.metalsharp/shader-cache/<pipeline>/<appid>/`
- M12 DLLs: `~/.metalsharp/runtime/wine/lib/dxmt_m12/`

## Backend API

The backend listens on `127.0.0.1:9274`; `METALSHARP_PORT` is validation-only. The router is `app/src-c/runtime/backend.c`. Domain implementations live beside it, including `steam_actions.c`, `bottles.c`, `setup.c`, `migration.c`, `epic.c`, `gog.c`, `gamejolt.c`, `pcsx2.c`, `rpcs3.c`, `shadps4.c`, and `sharpemu.c`.

Representative endpoints:

- `GET /status`
- `GET /steam/library`
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

### Electron

```bash
cd app
npm ci
npm run build
npm run pack
```

`npm run pack` builds the C backend and Electron app before packaging.

### Native engine

```bash
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build-native --parallel "$(sysctl -n hw.ncpu)"
ctest --test-dir build-native --output-on-failure
```

### DMG

```bash
tools/dmg/create-bundles.sh
cd app && npm run dmg
```

Do not rebuild the runtime archive unless explicitly requested.

## CI

- `pr-ci.yml`: shell, rules, docs, Metal, Vue, Electron, C backend, native, and DMG workflow checks.
- `ci.yml`: equivalent main-branch validation.
- `release.yml`: native build, C backend/Electron packaging, DMG verification, signing, and publication.

## Versioning

Keep these synchronized:

- `CMakeLists.txt`
- `app/src-c/Makefile`
- `app/package.json`
- `app/package-lock.json`

Use `tools/release/set-version.sh X.Y.Z`.

## Required Practices

- The packaged C backend is authoritative.
- Use port 9274 for normal operation; temporary ports are validation-only.
- Package and atomically install backend/frontend changes before final validation.
- Do not preview Electron against a temporary `METALSHARP_HOME`.
- Preserve firmware, saves, configuration, profiles, caches, and external games during emulator updates.
- Never log or persist launcher secrets.
- Never use AppleScript to quit a possibly closed MetalSharp app; addressing it launches it first.
- Leave `/Applications/MetalSharp.app` closed after final validation unless the user requests otherwise.
- Validate C formatting, C tests, TypeScript, packaging, hashes, and deep code signing for release-facing changes.
