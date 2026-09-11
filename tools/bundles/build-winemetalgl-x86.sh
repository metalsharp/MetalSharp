#!/usr/bin/env bash
# Build the WineMetalGL native sidecar for MetalSharp's x86_64-host Wine 11.17 lane.
# The Wine integration itself is built from the ported Wine 11.17 tree; this
# script only rebuilds the standalone sidecar from the pinned v0.1.0 release.
set -euo pipefail

OUT_DIR="${1:?usage: $0 OUTPUT_DIR}"
ROOT="${TMPDIR:-/tmp}/metalsharp-winemetalgl-${$}"
ARCHIVE="$ROOT/WineMetalGL-0.1.0.tar.zst"
SOURCE="$ROOT/WineMetalGL-0.1.0"
URL="https://github.com/metalsharp/WineMetalGL/releases/download/v0.1.0/WineMetalGL-0.1.0.tar.zst"
SHA256="1796266dc1fe43bb15050851d2a3f43d53c1ae961c7596b193cc9b47a465a553"
trap 'rm -rf "$ROOT"' EXIT
mkdir -p "$ROOT" "$OUT_DIR"
curl --fail --location --silent --show-error "$URL" -o "$ARCHIVE"
printf '%s  %s\n' "$SHA256" "$ARCHIVE" | shasum -a 256 -c -
tar --use-compress-program=unzstd -xf "$ARCHIVE" -C "$ROOT"

# v0.1.0's public preset hard-codes arm64. The sidecar is otherwise host
# independent, so make the host architecture an explicit local build input.
python3 - "$SOURCE/CMakeLists.txt" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
s = s.replace('set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Host architecture")',
              'set(WINEMETALGL_HOST_ARCH "x86_64" CACHE STRING "Host architecture")\n'
              'set(CMAKE_OSX_ARCHITECTURES "${WINEMETALGL_HOST_ARCH}" CACHE STRING "Host architecture" FORCE)')
s = s.replace('OSX_ARCHITECTURES "arm64"', 'OSX_ARCHITECTURES "${WINEMETALGL_HOST_ARCH}"')
p.write_text(s)
PY
MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.0}"
export MACOSX_DEPLOYMENT_TARGET
cmake -S "$SOURCE" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DWINEMETALGL_HOST_ARCH=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}"
cmake --build "$ROOT/build" --parallel "${JOBS:-6}"
install -m 0755 "$ROOT/build/metalsharp-opengl.dylib" "$OUT_DIR/metalsharp-opengl.dylib"
test "$(lipo -archs "$OUT_DIR/metalsharp-opengl.dylib")" = x86_64
shasum -a 256 "$OUT_DIR/metalsharp-opengl.dylib"
