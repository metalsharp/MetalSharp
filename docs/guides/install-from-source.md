# Install from Source
**Updated:** 2026-07-08


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

# Rust backend
cd app/src-rust && cargo build --release && cd ../..

# Electron frontend
cd app && npm install && npm run build && cd ..
```

## Fetch bootstrap bundles

The DMG/bootstrap build fetches the small Steam and Goldberg archives. The
complete VKMT-Wine runtime is downloaded and verified by the migration script
after installation, rather than embedded in the DMG.

GPTK/D3DMetal is not bundled in MetalSharp release assets. When you save a D3DMetal bottle, MetalSharp installs/trusts Homebrew GPTK separately and uses `/Applications/Game Porting Toolkit.app` directly.

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
- **Missing bootstrap bundles**: Run `./tools/dmg/create-bundles.sh` — this downloads the build inputs and generates `app/bundles/goldberg.tar.zst` plus the Steam bootstrap archive. VKMT-Wine is downloaded during migration. It does not download GPTK; D3DMetal uses Homebrew GPTK.
- **App won't open**: If you see a Gatekeeper warning, run `xattr -cr /path/to/MetalSharp.app`
