#!/usr/bin/env bash
# Package the release MetalSharp app with the production identity expected by
# app/updater/update.sh (com.metalsharp.app, Contents/MacOS/MetalSharp,
# MetalSharp.app). Release CI and tools/dmg/build-dmg.sh build on this.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# CMakeLists.txt is the version source of truth (tools/release/set-version.sh).
VERSION="$(sed -n 's/^project(metalsharp VERSION \([0-9][0-9.]*\) .*/\1/p' "$ROOT/CMakeLists.txt" | head -1)"
[ -n "$VERSION" ] || { echo "Could not read the version from CMakeLists.txt" >&2; exit 1; }

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
