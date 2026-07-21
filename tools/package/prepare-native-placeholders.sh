#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NATIVE_DIR="$PROJECT_ROOT/app/native"
HOST_DIR="$NATIVE_DIR/host"

mkdir -p "$NATIVE_DIR"
mkdir -p "$HOST_DIR"

# Detect host platform so we only validate platform-specific binaries that
# CMake is actually expected to build here. macOS produces .dylib (and bare
# executables), Linux produces .so, Windows produces .dll. Other files (the
# metalsharp_* binaries and host runtime libraries) are platform-agnostic in
# name but built per-host via CMake.
case "$(uname -s)" in
  Darwin)
    PLATFORM_SHLIB_EXT="dylib"
    PLATFORM_BIN_SUFFIX=""
    ;;
  Linux)
    PLATFORM_SHLIB_EXT="so"
    PLATFORM_BIN_SUFFIX=""
    ;;
  MINGW*|MSYS*|CYGWIN*|Windows*)
    PLATFORM_SHLIB_EXT="dll"
    PLATFORM_BIN_SUFFIX=".exe"
    ;;
  *)
    echo "WARNING: unknown host platform $(uname -s); defaulting to .dylib expectations" >&2
    PLATFORM_SHLIB_EXT="dylib"
    PLATFORM_BIN_SUFFIX=""
    ;;
esac

# Files that MUST be built from CMake before packaging. Do not create zero-byte
# stubs for these: if CMake has not produced them yet, surface the gap loudly so
# we never silently ship an empty shim. Validated per-platform below.
MUST_BUILD_BASE=(
  d3d11
  d3d12
  dxgi
  xaudio2_9
  xinput1_4
  opengl32
)
MUST_BUILD_EXECUTABLES=(
  metalsharp
  metalsharp_launcher
)
MUST_BUILD_HOST_RUNTIME=(
  libmetalsharp_host_runtime
)

# Files that come from external sources or are otherwise optional. We keep the
# legacy stub behavior so downstream tools that expect these to exist (even as
# placeholders) continue to work.
EXTERNAL_FILES=(
  metalsharp-process-manager-helper
  metalsharp-activate-pid
)

warn_missing() {
  local path="$1"
  local kind="$2"
  if [ ! -e "$path" ]; then
    echo "WARNING: must-build $kind missing: $path (will not create empty stub)" >&2
    return 0
  fi
  if [ ! -s "$path" ]; then
    echo "ERROR: must-build $kind is zero bytes: $path" >&2
    echo "  CMake post-build should have replaced this stub with a real binary." >&2
    return 1
  fi
  return 0
}

validate_must_build() {
  local errors=0

  # Platform-specific shim libraries: only the extension for the current host
  # is expected to exist. We deliberately do not require all three extensions
  # because CMake builds per-platform.
  for base in "${MUST_BUILD_BASE[@]}"; do
    case "$PLATFORM_SHLIB_EXT" in
      dylib) name="${base}.dylib" ;;
      so)    name="${base}.so" ;;
      dll)   name="${base}.dll" ;;
    esac
    if ! warn_missing "$NATIVE_DIR/$name" "shim"; then
      errors=$((errors + 1))
    fi
  done

  # Host executables: built per host. macOS/Linux get bare names, Windows
  # gets .exe suffix.
  for base in "${MUST_BUILD_EXECUTABLES[@]}"; do
    name="${base}${PLATFORM_BIN_SUFFIX}"
    if ! warn_missing "$NATIVE_DIR/$name" "executable"; then
      errors=$((errors + 1))
    fi
  done

  # Host runtime: also platform-specific in extension.
  for base in "${MUST_BUILD_HOST_RUNTIME[@]}"; do
    case "$PLATFORM_SHLIB_EXT" in
      dylib) name="${base}.dylib" ;;
      so)    name="${base}.so" ;;
      dll)   name="${base}.dll" ;;
    esac
    if ! warn_missing "$HOST_DIR/$name" "host runtime library"; then
      errors=$((errors + 1))
    fi
  done

  return "$errors"
}

# Stub the external/optional files that ship from outside the CMake tree.
# (Legacy behavior preserved so existing tooling keeps working.)
for file in "${EXTERNAL_FILES[@]}"; do
  if [ ! -e "$NATIVE_DIR/$file" ]; then
    : > "$NATIVE_DIR/$file"
  fi
done

# NOTE: must-build files (CMake shims, executables, host runtime) are no longer
# stubbed here. They must be produced by `cmake --build` plus the POST_BUILD
# copy commands in CMakeLists.txt. The validation below enforces that policy.

if ! validate_must_build; then
  echo "FAILED: app/native contains zero-byte must-build artifacts." >&2
  echo "  Run \`cmake --build build/<preset>\` before invoking this script." >&2
  exit 1
fi

if [ ! -f "$HOST_DIR/HostRuntimeABI.h" ]; then
  cp "$PROJECT_ROOT/include/metalsharp/HostRuntimeABI.h" "$HOST_DIR/HostRuntimeABI.h"
fi

if [ ! -f "$HOST_DIR/manifest.json" ]; then
  cat > "$HOST_DIR/manifest.json" <<'JSON'
{
  "abi": "metalsharp-host-runtime",
  "version": {
    "major": 1,
    "minor": 0
  },
  "services": [
    "process",
    "paths",
    "logging",
    "steam",
    "graphics",
    "audio",
    "input",
    "managed_runtime"
  ]
}
JSON
fi
