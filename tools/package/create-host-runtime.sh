#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${METALSHARP_BUILD_DIR:-$PROJECT_ROOT/build}"
HOST_DIR="${METALSHARP_HOST_RUNTIME_OUT:-$PROJECT_ROOT/app/native/host}"

mkdir -p "$HOST_DIR"

copy_if_present() {
  local src="$1"
  local dest="$2"
  if [ -f "$src" ] && [ -s "$src" ]; then
    cp "$src" "$dest"
    return 0
  fi
  return 1
}

copied=0
for candidate in \
  "$BUILD_DIR/libmetalsharp_host_runtime.dylib" \
  "$BUILD_DIR/libmetalsharp_host_runtime.so" \
  "$BUILD_DIR/metalsharp_host_runtime.dll"
do
  if copy_if_present "$candidate" "$HOST_DIR/$(basename "$candidate")"; then
    copied=1
  fi
done

if [ "$copied" -ne 1 ]; then
  echo "No metalsharp_host_runtime shared library found in $BUILD_DIR" >&2
  exit 1
fi

if [ "$(uname -s)" = "Darwin" ]; then
  "$SCRIPT_DIR/verify-macos-architecture.sh" arm64 "$HOST_DIR/libmetalsharp_host_runtime.dylib"
fi

cp "$PROJECT_ROOT/include/metalsharp/HostRuntimeABI.h" "$HOST_DIR/HostRuntimeABI.h"

cat > "$HOST_DIR/manifest.json" <<'JSON'
{
  "abi": "metalsharp-host-runtime",
  "version": {
    "major": 1,
    "minor": 0
  },
  "defaultSteamBridgePort": 18733,
  "env": {
    "steamBridgePort": "METALSHARP_STEAM_BRIDGE_PORT",
    "monoLib": "METALSHARP_MONO_LIB",
    "monoRoot": "METALSHARP_MONO_ROOT",
    "monoAssemblyDir": "METALSHARP_MONO_ASSEMBLY_DIR",
    "monoConfigDir": "METALSHARP_MONO_CONFIG_DIR"
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

echo "MetalSharp host runtime staged at $HOST_DIR"
find "$HOST_DIR" -maxdepth 1 -type f -print | sort
