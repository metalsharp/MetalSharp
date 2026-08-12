#!/usr/bin/env bash
set -euo pipefail

EXPECTED_ARCH="${1:?usage: verify-macos-architecture.sh ARCH BINARY}"
BINARY="${2:?usage: verify-macos-architecture.sh ARCH BINARY}"

case "$EXPECTED_ARCH" in
  arm64|x86_64) ;;
  *)
    echo "Unsupported expected macOS architecture: $EXPECTED_ARCH" >&2
    exit 2
    ;;
esac

if [ "$(uname -s)" != "Darwin" ]; then
  echo "SKIP: macOS architecture check for $BINARY on $(uname -s)"
  exit 0
fi

if [ ! -s "$BINARY" ]; then
  echo "Missing or empty macOS binary: $BINARY" >&2
  exit 1
fi

if ! command -v lipo >/dev/null 2>&1; then
  echo "lipo is required to validate macOS binary architectures" >&2
  exit 127
fi

info="$(lipo -info "$BINARY")"
archs="$(lipo -archs "$BINARY")"
echo "ARCH: $BINARY ($info)"

if [ "$archs" != "$EXPECTED_ARCH" ]; then
  echo "ERROR: $BINARY has architecture(s) '$archs'; expected only '$EXPECTED_ARCH'" >&2
  exit 1
fi
