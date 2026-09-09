# Install from Source
**Updated:** 2026-09-08


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
# Native engine (C++ D3D/Metal layer) - x86_64 for Rosetta 2 PE translation
mkdir -p build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build --parallel $(sysctl -n hw.ncpu)

# C backend
make -C app/src-c

# Electron frontend
cd app && npm install && npm run build && cd ..
```

## Fetch Runtime Bundles

Downloads MetalSharp-owned runtime assets from the GitHub release: Wine, DXMT/M12 graphics DLLs, Steam setup files, Mono/FNA support files, Goldberg assets, and other bundled runtime material.

The managed runtime includes Wine 11.17 and the GPTK 4 beta 2 D3DMetal payload. D3DMetal uses MetalSharp Wine and the shared Steam prefix; saving a bottle does not install a separate Homebrew GPTK app. See [Wine Architecture](../runtime/wine-architecture.md#d3dmetal).

Building the app does not compile Wine. To rebuild the patched Wine runtime itself, follow [How to Build MetalSharp Wine](how-to-build-metalsharp-wine.md); a prepared source tree and matching x86_64 dependencies are required.

```bash
./tools/dmg/create-bundles.sh
```

## Run

```bash
cd app && npx electron .
```

## Build a Signed App

For an ad-hoc signed `.app` (no Apple Developer account needed):

```bash
cd app && npx electron-builder --dir --mac --arm64
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
- **Missing bundles**: Run `./tools/dmg/create-bundles.sh` — this downloads MetalSharp-owned runtime assets from GitHub. Use the matched managed D3DMetal payload rather than mixing DLLs with a separate GPTK installation.
- **App won't open**: If you see a Gatekeeper warning, run `xattr -cr /path/to/MetalSharp.app`
