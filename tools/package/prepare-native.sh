#!/bin/bash
# One-shot native preparation for packaging.
#
# Used by `npm run prepare:native` (which backs `npm run pack` and
# `npm run dist`) and by the release workflow. On a fresh checkout this
# produces the complete app/native tree that electron-builder packages:
#
#   1. Configures and builds the native CMake tree (Release). The POST_BUILD
#      staging in CMakeLists.txt copies every must-build shim/executable into
#      app/native/ as each target builds.
#   2. Re-stages every must-build artifact from the build directory
#      unconditionally, so a warm build dir cannot silently skip staging
#      (POST_BUILD only runs when a target relinks).
#   3. Stages the host runtime (libmetalsharp_host_runtime + ABI header +
#      manifest) into app/native/host/.
#   4. Validates the staged tree and creates placeholders only for the
#      explicitly optional external files.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="${METALSHARP_PROJECT_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
BUILD_DIR="${METALSHARP_BUILD_DIR:-$PROJECT_ROOT/build}"
NATIVE_DIR="$PROJECT_ROOT/app/native"

# 1. Native engine (C++ D3D/Metal layer, x86_64 for Rosetta PE translation).
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
cmake --build "$BUILD_DIR" --parallel

# 2. Deterministic re-staging of the must-build artifacts produced by CMake.
#    Mirrors the platform expectations of prepare-native-placeholders.sh.
case "$(uname -s)" in
  Darwin)
    SHLIB_EXT="dylib"
    BIN_SUFFIX=""
    ;;
  Linux)
    SHLIB_EXT="so"
    BIN_SUFFIX=""
    ;;
  MINGW*|MSYS*|CYGWIN*|Windows*)
    SHLIB_EXT="dll"
    BIN_SUFFIX=".exe"
    ;;
  *)
    SHLIB_EXT="dylib"
    BIN_SUFFIX=""
    ;;
esac

mkdir -p "$NATIVE_DIR"
for base in d3d11 d3d12 dxgi xaudio2_9 xinput1_4 opengl32; do
  cp "$BUILD_DIR/$base.$SHLIB_EXT" "$NATIVE_DIR/$base.$SHLIB_EXT"
done
for base in metalsharp metalsharp_launcher; do
  cp "$BUILD_DIR/$base$BIN_SUFFIX" "$NATIVE_DIR/$base$BIN_SUFFIX"
done
# EAC substrate is a macOS/Rosetta-lane artifact. The Linux symbol image
# (metalsharp_eac_libc.so.6) is generated directly into app/native/ by a
# CMake custom target during `cmake --build`, so it needs no copy here.
if [ "$SHLIB_EXT" = "dylib" ]; then
  cp "$BUILD_DIR/metalsharp_eac_substrate.dylib" "$NATIVE_DIR/metalsharp_eac_substrate.dylib"
  cp "$BUILD_DIR/MetalSharpMigrator" "$NATIVE_DIR/MetalSharpMigrator"
fi

# 3. Host runtime shared library + ABI header + manifest.
METALSHARP_BUILD_DIR="$BUILD_DIR" "$SCRIPT_DIR/create-host-runtime.sh"

# 4. Validate must-build artifacts; stub only the optional external files.
"$SCRIPT_DIR/prepare-native-placeholders.sh"

echo "Native tree prepared at $NATIVE_DIR"
