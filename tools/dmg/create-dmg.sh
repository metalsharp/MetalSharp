#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
DMG_NAME="MetalSharp-0.1.1"
DMG_DIR="$PROJECT_DIR/dist/$DMG_NAME"
DMG_OUTPUT="$PROJECT_DIR/dist/MetalSharp-0.1.1.dmg"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

info() { echo -e "${CYAN}[dmg]${NC} $*"; }
ok()   { echo -e "${GREEN}[ok]${NC} $*"; }
fail() { echo -e "${RED}[fail]${NC} $*"; exit 1; }

if [[ "$(uname)" != "Darwin" ]]; then
    fail "DMG creation requires macOS"
fi

if ! command -v hdiutil &>/dev/null; then
    fail "hdiutil not found. Install Xcode CLI tools: xcode-select --install"
fi

info "Building MetalSharp..."
cd "$PROJECT_DIR"
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF 2>&1 | tail -3
cmake --build build 2>&1 | tail -5

info "Creating DMG staging directory..."
rm -rf "$DMG_DIR"
mkdir -p "$DMG_DIR"

info "Copying binaries..."
mkdir -p "$DMG_DIR/bin"
for binary in metalsharp metalsharp_launcher; do
    if [[ -f "$BUILD_DIR/$binary" ]]; then
        cp "$BUILD_DIR/$binary" "$DMG_DIR/bin/"
        ok "  $binary"
    fi
done

mkdir -p "$DMG_DIR/lib"
for dylib in d3d11.dylib d3d12.dylib dxgi.dylib xaudio2_9.dylib xinput1_4.dylib opengl32.dylib; do
    if [[ -f "$BUILD_DIR/$dylib" ]]; then
        cp "$BUILD_DIR/$dylib" "$DMG_DIR/lib/"
        ok "  $dylib"
    fi
done

info "Copying documentation..."
mkdir -p "$DMG_DIR/docs"
cp "$PROJECT_DIR/README.md" "$DMG_DIR/"
cp "$PROJECT_DIR/LICENSE" "$DMG_DIR/" 2>/dev/null || true
cp "$PROJECT_DIR/docs/"*.md "$DMG_DIR/docs/"

info "Copying installer..."
cp "$PROJECT_DIR/install.sh" "$DMG_DIR/"
chmod +x "$DMG_DIR/install.sh"

info "Creating wrapper script..."
cat > "$DMG_DIR/metalsharp.sh" << 'WRAPPER'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export DYLD_LIBRARY_PATH="$SCRIPT_DIR/lib:${DYLD_LIBRARY_PATH:-}"
exec "$SCRIPT_DIR/bin/metalsharp" "$@"
WRAPPER
chmod +x "$DMG_DIR/metalsharp.sh"

info "Writing install notes..."
cat > "$DMG_DIR/INSTALL.txt" << 'NOTES'
MetalSharp 0.1.1 — D3D to Metal Translation Layer

Quick Install:
  1. Open Terminal
  2. cd to this directory
  3. Run: ./install.sh

Manual Install:
  1. Copy bin/metalsharp and bin/metalsharp_launcher to /usr/local/bin/
  2. Copy lib/*.dylib to ~/.metalsharp/prefix/drive_c/windows/system32/
  3. Run: metalsharp path/to/game.exe

Requirements:
  - macOS 13+ (Ventura or later)
  - Xcode Command Line Tools
  - Metal-capable GPU (all Apple Silicon, most Intel Macs 2012+)

Documentation:
  - docs/USER-GUIDE.md — Getting started
  - docs/COMPATIBILITY.md — Game compatibility
  - docs/TROUBLESHOOTING.md — Common issues
  - docs/DEVELOPER-GUIDE.md — Contributing

https://github.com/aaf2tbz/metalsharp
NOTES

info "Creating DMG..."
mkdir -p "$(dirname "$DMG_OUTPUT")"
rm -f "$DMG_OUTPUT"

hdiutil create -volname "MetalSharp" \
    -srcfolder "$DMG_DIR" \
    -ov -format UDZO \
    "$DMG_OUTPUT"

ok "DMG created: $DMG_OUTPUT"
info "Size: $(du -sh "$DMG_OUTPUT" | cut -f1)"

rm -rf "$DMG_DIR"
ok "Cleaned up staging directory"
