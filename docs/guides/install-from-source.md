# Install from Source
**Updated:** 2026-08-11


Build MetalSharp from source without using the DMG. Requires macOS 14+ on Apple Silicon.

## Prerequisites

```bash
# Xcode CLI Tools
xcode-select --install

# Homebrew
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
eval "$(/opt/homebrew/bin/brew shellenv)"

# Build dependencies
brew install cmake node zstd
```

## Clone

```bash
git clone --recurse-submodules https://github.com/aaf2tbz/metalsharp.git
cd metalsharp
```

## Build

```bash
# Wine-facing native engine (C++ D3D/Metal layer) - x86_64 for Rosetta 2 PE translation
# Host helpers, host runtime, and the update migrator are built arm64 for the app.
mkdir -p build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build --parallel $(sysctl -n hw.ncpu)

# Rust backend
cd app/src-rust && cargo build --release && cd ../..

# Electron frontend
cd app && npm install && npm run build && cd ..
```

## Prepare the Native Package Tree

`npm run pack` / `npm run dist` (and the release workflow) run
`npm run prepare:native` automatically. For a standalone one-shot native
preparation — CMake configure+build, staging of every shim/executable into
`app/native/` via the CMake POST_BUILD commands, host runtime, and must-build
validation — run:

```bash
tools/package/prepare-native.sh
```

The script fails if any must-build artifact is missing, so a fresh checkout
can never be packaged without the native surface.

## Fetch Runtime Bundles

Downloads MetalSharp-owned runtime assets from the GitHub release: Wine, graphics DLLs (DXMT for DXMT/DXMT(32), vkd3d-proton + DXVK-macOS + VKMT MoltenVK for the VKD3D route), Steam setup files, Mono/FNA support files, Goldberg assets, and other bundled runtime material.

GPTK/D3DMetal is not bundled in MetalSharp release assets. When you save a D3DMetal bottle, MetalSharp installs/trusts Homebrew GPTK separately and uses `/Applications/Game Porting Toolkit.app` directly.

```bash
./tools/dmg/create-bundles.sh
```

## Run

```bash
cd app && npx electron .
```

The Electron main process starts the Rust backend with a fresh per-session
bearer token and attaches it to every backend request. Run the application
through Electron rather than exposing `metalsharp-backend` directly; requests
without that token are rejected before route handling. The backend binds only
to loopback, and browser-origin requests are limited to the Vite development
origins used by this checkout.

## Build a Signed App

For an ad-hoc signed `.app` (no Apple Developer account needed):

```bash
cd app && npm run pack
codesign --force --deep --sign - ../dist/electron/mac-arm64/MetalSharp.app
open ../dist/electron/mac-arm64/MetalSharp.app
```

For a distributable DMG with hardened runtime (requires Apple Developer certificate):

```bash
cd app && npm run dmg
```

## Troubleshooting

- **`cmake` fails**: Ensure Xcode CLI tools are installed (`xcode-select -p` should return a path)
- **`npm install` fails**: Make sure Node 18+ is installed (`brew install node`)
- **Missing bundles**: Run `./tools/dmg/create-bundles.sh` — this downloads MetalSharp-owned runtime assets from GitHub. It does not download GPTK; D3DMetal uses Homebrew GPTK.
- **App won't open**: If you see a Gatekeeper warning, run `xattr -cr /path/to/MetalSharp.app`
