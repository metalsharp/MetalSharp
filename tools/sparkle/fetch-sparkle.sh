#!/bin/bash
# Sparkle (https://sparkle-project.org), checksum-pinned, unpacked once into build/sparkle/<version>:
#   Sparkle.framework   embedded in MetalSharp.app/Contents/Frameworks
#   bin/                BinaryDelta, sign_update, generate_keys, generate_appcast (release tooling)
# Prints that directory. SPARKLE_DIR overrides where it goes.
set -euo pipefail

SPARKLE_VERSION="2.9.6"
SPARKLE_SHA256="52bf9e88cdd972fc0c81501377a880e90d47031bd8ca5462488f843e2609e192"
SPARKLE_URL="https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEST="${SPARKLE_DIR:-$ROOT/build/sparkle/$SPARKLE_VERSION}"

if [ -d "$DEST/Sparkle.framework" ] && [ -x "$DEST/bin/BinaryDelta" ]; then
  echo "$DEST"
  exit 0
fi

archive="$(mktemp -t sparkle).tar.xz"
trap 'rm -f "$archive"' EXIT
curl --fail --location --silent --show-error --retry 3 -o "$archive" "$SPARKLE_URL"
actual="$(shasum -a 256 "$archive" | awk '{print $1}')"
if [ "$actual" != "$SPARKLE_SHA256" ]; then
  echo "Sparkle-${SPARKLE_VERSION}.tar.xz sha256 $actual, expected $SPARKLE_SHA256" >&2
  exit 1
fi
rm -rf "$DEST.partial"
mkdir -p "$DEST.partial"
tar -xJf "$archive" -C "$DEST.partial"
test -d "$DEST.partial/Sparkle.framework" && test -x "$DEST.partial/bin/BinaryDelta"
rm -rf "$DEST"
mv "$DEST.partial" "$DEST"
echo "$DEST"
