#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
APP_DIR="$PROJECT_DIR/app"
BUILD_DIR="$PROJECT_DIR/dist/electron"
DMG_OUTPUT="$PROJECT_DIR/dist/MetalSharp.dmg"
RUNTIME_DIR="$PROJECT_DIR/src/fna"
VER=$(grep '"version"' "$APP_DIR/package.json" | head -1 | sed 's/.*: *"//;s/".*//')

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

info "Building C backend..."
make -C "$APP_DIR/src-c"

info "Copying runtime files to ~/.metalsharp/runtime/..."
HOME_DIR="$HOME"
mkdir -p "$HOME_DIR/.metalsharp/runtime/fna"
mkdir -p "$HOME_DIR/.metalsharp/runtime/shims"

if [[ -d "$RUNTIME_DIR/shims" ]]; then
    for f in "$RUNTIME_DIR/shims/"*.dylib; do
        if [[ -f "$f" ]]; then
            cp "$f" "$HOME_DIR/.metalsharp/runtime/shims/"
            ok "  $(basename "$f")"
        fi
    done
fi

if [[ -f "$HOME_DIR/.metalsharp/games/celeste-fna/libFNA3D.dylib" ]]; then
    cp "$HOME_DIR/.metalsharp/games/celeste-fna/libFNA3D.dylib" "$HOME_DIR/.metalsharp/runtime/shims/"
    ok "  libFNA3D.dylib"
fi

if [[ -f "$HOME_DIR/.metalsharp/games/celeste-fna/libSDL3.dylib" ]]; then
    cp "$HOME_DIR/.metalsharp/games/celeste-fna/libSDL3.dylib" "$HOME_DIR/.metalsharp/runtime/shims/"
    ok "  libSDL3.dylib"
fi

if [[ -f "$HOME_DIR/.metalsharp/games/celeste-fna/libsteam_api.dylib" ]]; then
    cp "$HOME_DIR/.metalsharp/games/celeste-fna/libsteam_api.dylib" "$HOME_DIR/.metalsharp/runtime/shims/"
    ok "  libsteam_api.dylib"
fi

FNA_DLL_DIR="$PROJECT_DIR/src/fna/FNA/bin/Release/net4.0"
if [[ -d "$FNA_DLL_DIR" ]]; then
    for f in "$FNA_DLL_DIR/"*.dll "$FNA_DLL_DIR/"*.config; do
        if [[ -f "$f" ]]; then
            cp "$f" "$HOME_DIR/.metalsharp/runtime/fna/"
            ok "  fna/$(basename "$f")"
        fi
    done
fi

info "Building Electron app..."
cd "$APP_DIR"
npm run build 2>&1 | tail -5

info "Packaging with electron-builder..."
npx electron-builder --mac dmg --arm64 2>&1 | tail -10

if [[ -f "$BUILD_DIR/MetalSharp-${VER}-arm64.dmg" ]]; then
    cp "$BUILD_DIR/MetalSharp-${VER}-arm64.dmg" "$DMG_OUTPUT"
    ok "DMG created: $DMG_OUTPUT"
    info "Size: $(du -sh "$DMG_OUTPUT" | cut -f1)"
elif [[ -f "$BUILD_DIR/MetalSharp-${VER}.dmg" ]]; then
    cp "$BUILD_DIR/MetalSharp-${VER}.dmg" "$DMG_OUTPUT"
    ok "DMG created: $DMG_OUTPUT"
    info "Size: $(du -sh "$DMG_OUTPUT" | cut -f1)"
else
    fail "DMG not found in $BUILD_DIR"
fi

ok "Done! Users can mount the DMG and drag MetalSharp to Applications."
