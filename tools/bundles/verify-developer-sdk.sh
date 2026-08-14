#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
ARCHIVE="${1:-$PROJECT_ROOT/dist/developer-sdk/metalsharp-d3d12-developer-sdk.tar.zst}"
ROOT="developer-sdk/d3d12"

if [ ! -s "$ARCHIVE" ]; then
  echo "Missing developer SDK archive: $ARCHIVE" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-developer-sdk-verify.XXXXXX")"
cleanup() {
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

tar --use-compress-program=unzstd -xf "$ARCHIVE" -C "$TMP_DIR"

required=(
  "$ROOT/README.md"
  "$ROOT/docs/developer-runtime.md"
  "$ROOT/runtime/README.md"
  "$ROOT/runtime/manifest.json"
  "$ROOT/runtime/wine/bin/wine"
  "$ROOT/runtime/dxmt/x86_64-windows/d3d12.dll"
  "$ROOT/runtime/dxmt/x86_64-windows/dxgi.dll"
  "$ROOT/runtime/dxmt/x86_64-windows/dxgi_dxmt.dll"
  "$ROOT/runtime/dxmt/x86_64-windows/winemetal.dll"
  "$ROOT/runtime/dxmt/x86_64-unix/winemetal.so"
  "$ROOT/runtime/dxmt_vkd3d/x86_64-windows/d3d12.dll"
  "$ROOT/runtime/dxmt_vkd3d/x86_64-windows/dxgi.dll"
  "$ROOT/runtime/dxmt_vkd3d/x86_64-windows/dxgi_dxmt.dll"
  "$ROOT/runtime/dxmt_vkd3d/x86_64-windows/winemetal.dll"
  "$ROOT/runtime/dxmt_vkd3d/x86_64-unix/winemetal.so"
  "$ROOT/runtime/dxmt_vkd3d/x86_64-unix/libc++.1.dylib"
  "$ROOT/runtime/dxmt_vkd3d/x86_64-unix/libc++abi.1.dylib"
  "$ROOT/runtime/dxmt_vkd3d/x86_64-unix/libunwind.1.dylib"
  "$ROOT/scripts/run-probes.sh"
  "$ROOT/scripts/stage-dxmt-runtime.py"
  "$ROOT/scripts/preflight-runtime-layout.py"
  "$ROOT/scripts/sdk-env.sh"
  "$ROOT/scripts/sdk-env.ps1"
  "$ROOT/contracts/d3d12-metal-contract.json"
)

for path in "${required[@]}"; do
  if [ ! -s "$TMP_DIR/$path" ]; then
    echo "Developer SDK archive is missing required file: $path" >&2
    exit 1
  fi
done

while IFS=$'\t' read -r rel expected; do
  case "$rel" in
    ""|"#"*|path) continue ;;
  esac
  path="$TMP_DIR/$ROOT/runtime/dxmt_vkd3d/$rel"
  if [ ! -s "$path" ]; then
    echo "Developer SDK archive is missing VKD3D hash-checked file: runtime/dxmt_vkd3d/$rel" >&2
    exit 1
  fi
  actual="$(shasum -a 256 "$path" | awk '{print $1}')"
  if [ "$actual" != "$expected" ]; then
    echo "Developer SDK VKD3D hash mismatch: runtime/dxmt_vkd3d/$rel expected=$expected actual=$actual" >&2
    exit 1
  fi
done < "$PROJECT_ROOT/tools/bundles/vkd3d-dxmt-runtime-hashes.tsv"

python3 - "$TMP_DIR/$ROOT/runtime/manifest.json" <<'PY'
import json
import sys
from pathlib import Path

manifest = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
if manifest.get("schema") != "metalsharp.d3d12-developer-sdk.runtime.v1":
    raise SystemExit("unexpected developer runtime manifest schema")
if not manifest.get("ok"):
    raise SystemExit(f"developer runtime manifest is not ok: {manifest.get('missing')}")
for platform in ("macos", "linux", "windows"):
    if platform not in manifest.get("platforms", {}):
        raise SystemExit(f"missing platform note: {platform}")
PY

echo "Developer SDK verified: $ARCHIVE"
