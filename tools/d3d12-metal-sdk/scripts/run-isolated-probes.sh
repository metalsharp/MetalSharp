#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SDK_DIR="${ROOT_DIR}/tools/d3d12-metal-sdk"
RESULTS_DIR="${SDK_DIR}/results"
WINE_ROOT="${METALSHARP_WINE_ROOT:-${HOME}/.metalsharp/runtime/wine}"
WINE_BIN="${WINE_ROOT}/bin/wine"
WINESERVER_BIN="${WINE_ROOT}/bin/wineserver"
DXMT_RUNTIME="${METALSHARP_DXMT_RUNTIME:-${WINE_ROOT}/lib/dxmt_m12}"
PROFILE="${METALSHARP_PROBE_PROFILE:-metalsharp-isolated}"
EXPECTED_WINE_VERSION="${METALSHARP_EXPECTED_WINE_VERSION:-wine-11.5}"

usage() {
  cat <<'EOF'
Usage: run-isolated-probes.sh [run-probes.sh options]

Runs the D3D12 Metal SDK with MetalSharp's vendored Wine 11.5, an explicit
DXMT M12 runtime, and a disposable Wine prefix. The prefix is stopped and
removed on success, failure, or interruption.

Environment overrides:
  METALSHARP_WINE_ROOT         Wine runtime root (default ~/.metalsharp/runtime/wine)
  METALSHARP_DXMT_RUNTIME      DXMT runtime root (default <wine>/lib/dxmt_m12)
  METALSHARP_PROBE_PROFILE     Result profile (default metalsharp-isolated)
  METALSHARP_EXPECTED_WINE_VERSION
                               Required `wine --version` output (default wine-11.5)

The wrapper owns --profile, --wine, --prefix, and --dxmt-runtime; passing those
options is rejected so a gate cannot silently use another Wine or a persistent
prefix.
EOF
}

for arg in "$@"; do
  case "$arg" in
    -h|--help)
      usage
      exit 0
      ;;
    --profile|--profile=*|--wine|--wine=*|--prefix|--prefix=*|--dxmt-runtime|--dxmt-runtime=*)
      echo "error: $arg is owned by run-isolated-probes.sh" >&2
      exit 2
      ;;
  esac
done

if [[ ! -x "$WINE_BIN" ]]; then
  echo "error: MetalSharp Wine is not executable: $WINE_BIN" >&2
  exit 2
fi
if [[ ! -x "$WINESERVER_BIN" ]]; then
  echo "error: MetalSharp wineserver is not executable: $WINESERVER_BIN" >&2
  exit 2
fi
if [[ ! -d "$DXMT_RUNTIME/x86_64-windows" || ! -d "$DXMT_RUNTIME/x86_64-unix" ]]; then
  echo "error: DXMT runtime is incomplete: $DXMT_RUNTIME" >&2
  exit 2
fi

wine_version="$($WINE_BIN --version)"
if [[ "$wine_version" != "$EXPECTED_WINE_VERSION" ]]; then
  echo "error: expected MetalSharp $EXPECTED_WINE_VERSION, got $wine_version from $WINE_BIN" >&2
  exit 2
fi

mkdir -p "$RESULTS_DIR"
work_dir="$(mktemp -d /private/tmp/metalsharp-d3d12-probe.XXXXXX)"
prefix="$work_dir/prefix"
cleanup_started=0
cleanup() {
  status=$?
  if [[ "$cleanup_started" == "0" ]]; then
    cleanup_started=1
    WINEPREFIX="$prefix" "$WINESERVER_BIN" -k >/dev/null 2>&1 || true
    WINEPREFIX="$prefix" "$WINESERVER_BIN" -w >/dev/null 2>&1 || true
    rm -rf "$work_dir"
    if [[ -e "$work_dir" ]]; then
      echo "error: failed to remove isolated probe directory: $work_dir" >&2
      status=1
    else
      echo "isolated probe prefix removed: $prefix"
    fi
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

mkdir -p "$prefix"
WINEPREFIX="$prefix" WINEDEBUG=-all "$WINE_BIN" wineboot -u >/dev/null 2>&1
WINEPREFIX="$prefix" "$WINESERVER_BIN" -w >/dev/null 2>&1 || true

# The ABI probe checks the prefix copy in addition to the app-local runtime.
mkdir -p "$prefix/drive_c/windows/system32"
cp "$DXMT_RUNTIME/x86_64-windows/winemetal.dll" \
  "$prefix/drive_c/windows/system32/winemetal.dll"

xcode_version="unknown"
metal_version="unknown"
if [[ -n "${DEVELOPER_DIR:-}" && -x "${DEVELOPER_DIR}/usr/bin/xcodebuild" ]]; then
  xcode_version="$("${DEVELOPER_DIR}/usr/bin/xcodebuild" -version 2>/dev/null | tr '\n' ' ')"
fi
if command -v xcrun >/dev/null 2>&1; then
  metal_version="$(xcrun metal --version 2>/dev/null | head -1 || true)"
fi

ISO_DXMT_RUNTIME="$DXMT_RUNTIME" \
ISO_PROFILE="$PROFILE" \
ISO_WINE_BIN="$WINE_BIN" \
ISO_WINE_VERSION="$wine_version" \
ISO_WINESERVER_BIN="$WINESERVER_BIN" \
ISO_XCODE_VERSION="$xcode_version" \
ISO_METAL_VERSION="$metal_version" \
ISO_PREFIX="$prefix" \
python3 - "$RESULTS_DIR/isolated-probe-environment-$PROFILE.json" <<'PY'
import hashlib
import json
import os
import pathlib
import sys

out = pathlib.Path(sys.argv[1])
runtime = pathlib.Path(os.environ["ISO_DXMT_RUNTIME"])

def record(path):
    path = pathlib.Path(path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
    return {"path": str(path), "exists": path.exists(), "sha256": digest}

payload = {
    "schema": "metalsharp.d3d12-metal.isolated-probe-environment.v1",
    "profile": os.environ["ISO_PROFILE"],
    "wine": record(os.environ["ISO_WINE_BIN"]),
    "wine_version": os.environ["ISO_WINE_VERSION"],
    "wineserver": record(os.environ["ISO_WINESERVER_BIN"]),
    "dxmt_runtime": str(runtime),
    "d3d12": record(runtime / "x86_64-windows" / "d3d12.dll"),
    "dxgi": record(runtime / "x86_64-windows" / "dxgi.dll"),
    "dxgi_dxmt": record(runtime / "x86_64-windows" / "dxgi_dxmt.dll"),
    "winemetal_pe": record(runtime / "x86_64-windows" / "winemetal.dll"),
    "winemetal_unix": record(runtime / "x86_64-unix" / "winemetal.so"),
    "xcode": os.environ["ISO_XCODE_VERSION"],
    "metal": os.environ["ISO_METAL_VERSION"],
    "temporary_prefix": os.environ["ISO_PREFIX"],
}
out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(out)
PY

"$SDK_DIR/scripts/run-probes.sh" \
  --profile "$PROFILE" \
  --wine "$WINE_BIN" \
  --prefix "$prefix" \
  --dxmt-runtime "$DXMT_RUNTIME" \
  "$@"
