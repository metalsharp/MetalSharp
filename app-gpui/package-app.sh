#!/usr/bin/env bash
# Package the GPUI app with the production MetalSharp identity used by the
# Electron app and expected by app/updater/update.sh (com.metalsharp.app,
# Contents/MacOS/MetalSharp, MetalSharp.app).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(node -p "require('$ROOT/app/package.json').version" 2>/dev/null || sed -n 's/.*"version": "\(.*\)".*/\1/p' "$ROOT/app/package.json" | head -1)"

export METALSHARP_GPUI_APP_BUNDLE_NAME="MetalSharp.app"
export METALSHARP_GPUI_APP_DISPLAY_NAME="MetalSharp"
export METALSHARP_GPUI_APP_BUNDLE_ID="com.metalsharp.app"
export METALSHARP_GPUI_APP_EXECUTABLE="MetalSharp"
export METALSHARP_GPUI_APP_VERSION="${METALSHARP_GPUI_APP_VERSION:-$VERSION}"
export METALSHARP_GPUI_CARGO_PROFILE=release
export METALSHARP_GPUI_PACKAGE_BACKEND=1
export METALSHARP_GPUI_BUNDLE_SOURCE="${METALSHARP_GPUI_BUNDLE_SOURCE:-$ROOT/app/bundles}"
export METALSHARP_GPUI_REQUIRE_BUNDLES="${METALSHARP_GPUI_REQUIRE_BUNDLES:-1}"

exec "$ROOT/app-gpui/package-local-preview.sh"
