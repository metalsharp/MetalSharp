# Install from Source
**Updated:** 2026-10-04


Build MetalSharp from source without using the DMG. Requires macOS 14+ on Apple Silicon.

## Prerequisites

```bash
# Xcode CLI Tools
xcode-select --install

# Homebrew
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
eval "$(/opt/homebrew/bin/brew shellenv)"

# Build dependencies
brew install cmake zstd

# Rust toolchain (the app is Rust/GPUI)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

Packaging the `.app` also needs full Xcode (for `actool`, which compiles the Liquid Glass app icon).

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

# GPUI app
cargo build --release --manifest-path app-gpui/Cargo.toml
```

## Fetch Runtime Bundles

Downloads MetalSharp-owned runtime assets from the GitHub release: Wine, DXMT graphics DLLs, Steam setup files, Mono/FNA support files, Goldberg assets, and other bundled runtime material.

The managed runtime includes Wine 11.17 and the managed D3DMetal payload. D3DMetal uses MetalSharp Wine and the shared Steam prefix. See [Wine Architecture](../runtime/wine-architecture.md#d3dmetal).

Building the app does not compile Wine. To rebuild the patched Wine runtime itself, follow [How to Build MetalSharp Wine](how-to-build-metalsharp-wine.md); a prepared source tree and matching x86_64 dependencies are required.

```bash
./tools/dmg/create-bundles.sh
```

## Run

```bash
cargo run --release --manifest-path app-gpui/Cargo.toml
```

This starts the app against `~/.metalsharp` on port 9274, like the installed app. Quit any other MetalSharp first.

## Build a Signed App

For an ad-hoc signed `.app` (no Apple Developer account needed):

```bash
ACTOOL=/Applications/Xcode.app/Contents/Developer/usr/bin/actool \
CARGO_TARGET_DIR=/tmp/metalsharp-target \
  app-gpui/package-app.sh
open app-gpui/target/MetalSharp.app
```

Keep `CARGO_TARGET_DIR` on the internal disk; release builds can fail when it is on an external volume.

For a DMG (`MetalSharp-<version>-arm64.dmg`):

```bash
tools/dmg/package-gpui-dmg.sh app-gpui/target/MetalSharp.app dist/gpui
```

For a Developer ID signed, hardened-runtime app, set `METALSHARP_GPUI_SIGN_IDENTITY` to your "Developer ID Application" identity when running `package-app.sh`; see [Release Signing](../release/release-signing.md).

## Troubleshooting

- **`cmake` fails**: Ensure Xcode CLI tools are installed (`xcode-select -p` should return a path)
- **Release build fails with `can't find crate`**: Put `CARGO_TARGET_DIR` on the internal disk
- **Missing bundles**: Run `./tools/dmg/create-bundles.sh` — this downloads MetalSharp-owned runtime assets from GitHub. Use the matched managed D3DMetal payload with the matching runtime.
- **App won't open**: If you see a Gatekeeper warning, run `xattr -cr /path/to/MetalSharp.app`
