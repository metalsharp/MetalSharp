#!/usr/bin/env bash
# Local release-style DMG: package the GPUI MetalSharp.app with its backend and
# runtime bundles, wrap it in the distributable DMG and verify the payload.
# Ad-hoc signed unless METALSHARP_GPUI_SIGN_IDENTITY names a Developer ID
# Application identity. Bundles come from app/bundles (tools/dmg/create-bundles.sh).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUT_DIR="${METALSHARP_DMG_OUT_DIR:-$PROJECT_DIR/dist/gpui}"

if [[ "$(uname)" != "Darwin" ]]; then
  echo "DMG creation requires macOS" >&2
  exit 1
fi

"$PROJECT_DIR/app-gpui/package-app.sh"
"$SCRIPT_DIR/package-gpui-dmg.sh" "$PROJECT_DIR/app-gpui/target/MetalSharp.app" "$OUT_DIR"
for dmg in "$OUT_DIR"/MetalSharp-*-arm64.dmg; do
  "$SCRIPT_DIR/verify-dmg-runtime-assets.sh" "$dmg"
done
