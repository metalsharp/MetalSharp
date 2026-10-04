#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Separate bundle identity keeps this test build alongside the official app.
export METALSHARP_GPUI_APP_BUNDLE_NAME="MetalSharp-GPUI-Test.app"
export METALSHARP_GPUI_APP_DISPLAY_NAME="MetalSharp GPUI Test"
export METALSHARP_GPUI_APP_BUNDLE_ID="dev.metalsharp.gpui-test"
export METALSHARP_GPUI_APP_VERSION="${METALSHARP_GPUI_TEST_VERSION:-0.77.0}"
export METALSHARP_GPUI_APP_BUILD_VERSION="${METALSHARP_GPUI_TEST_BUILD_VERSION:-${METALSHARP_GPUI_APP_VERSION%%-*}}"
# Production behaviour: real ~/.metalsharp data home and backend port 9274.
export METALSHARP_GPUI_CARGO_PROFILE="${METALSHARP_GPUI_CARGO_PROFILE:-release}"
export METALSHARP_GPUI_PACKAGE_BACKEND=1
export METALSHARP_GPUI_BUNDLE_SOURCE="${METALSHARP_GPUI_BUNDLE_SOURCE:-$ROOT/dist/bundles}"
export METALSHARP_GPUI_REQUIRE_BUNDLES=1

exec "$ROOT/app-gpui/package-local-preview.sh"
