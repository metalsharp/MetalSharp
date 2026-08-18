#!/usr/bin/env bash
# Required Notice: Copyright (c) 2026 MetalSharp. Commercial licensing: averyfelts@aol.com
#
# Regenerate the pinned Vulkan-lane bundle hashes from the release archives:
#
#   tools/bundles/moltenvk-runtime-hashes.tsv     <- metalsharp-runtime.tar.zst
#   tools/bundles/vkd3d-proton-runtime-hashes.tsv <- metalsharp-graphics-dll.tar.zst
#   tools/bundles/dxvk-runtime-hashes.tsv         <- metalsharp-graphics-dll.tar.zst
#
# Run AFTER re-staging the MoltenVK dylib/ICD payloads or the vkd3d-proton /
# dxvk lane DLLs and BEFORE publishing the bundle, so the .tsv manifests and
# app/src-rust/src/installer.rs pins always match the published archives
# (CI verify-bundles.sh + the installer's archive hash gates enforce them).
#
# Usage:
#   tools/bundles/update-vulkan-lane-hashes.sh [--bundle-dir DIR]
#       [--sync-rust] [--check]
#
# Options:
#   --bundle-dir DIR   Directory with metalsharp-runtime.tar.zst and
#                      metalsharp-graphics-dll.tar.zst (default app/bundles).
#   --sync-rust        Also rewrite the pinned SHA-256 constants in
#                      app/src-rust/src/installer.rs and run cargo fmt.
#                      Only the MoltenVK constants and the VKD3D_REQUIRED_PE /
#                      DXVK_REQUIRED_PE (non-test) blocks are touched; the
#                      DXMT lanes (dxmt/dxmt-m12) and #[cfg(test)] fixture
#                      hashes are left alone.
#   --check            Regenerate into temp files and exit 1 on any hash
#                      drift (no writes, no rust sync).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUNDLE_DIR="$ROOT_DIR/app/bundles"
SYNC_RUST=0
CHECK=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --bundle-dir) BUNDLE_DIR="$2"; shift 2 ;;
    --sync-rust) SYNC_RUST=1; shift ;;
    --check) CHECK=1; shift ;;
    -h|--help) head -40 "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

RUNTIME_ARCHIVE="$BUNDLE_DIR/metalsharp-runtime.tar.zst"
GRAPHICS_ARCHIVE="$BUNDLE_DIR/metalsharp-graphics-dll.tar.zst"

[ -s "$RUNTIME_ARCHIVE" ] || { echo "missing: $RUNTIME_ARCHIVE" >&2; exit 2; }
[ -s "$GRAPHICS_ARCHIVE" ] || { echo "missing: $GRAPHICS_ARCHIVE" >&2; exit 2; }

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ms-vulkan-hashes.XXXXXX")"
trap '/bin/rm -rf "$TMP_DIR"' EXIT

# Extract the pinned members, then hash through the filesystem so tar
# symlinks (e.g. libMoltenVK.dylib -> libMoltenVK.1.dylib) resolve like the
# real install layout.
mkdir -p "$TMP_DIR/rt" "$TMP_DIR/gf"
tar --use-compress-program=unzstd -xf "$RUNTIME_ARCHIVE" -C "$TMP_DIR/rt" \
  runtime/wine/lib/wine/x86_64-unix/libMoltenVK.dylib \
  runtime/wine/lib/wine/x86_64-unix/libMoltenVK.1.dylib \
  runtime/wine/lib/moltenvk-vkmt/libMoltenVK.dylib \
  runtime/wine/lib/moltenvk-vkmt/libMoltenVK.1.dylib \
  runtime/wine/lib/moltenvk-vkmt/MoltenVK_icd.json \
  runtime/wine/etc/vulkan/icd.d/MoltenVK_icd.json
tar --use-compress-program=unzstd -xf "$GRAPHICS_ARCHIVE" -C "$TMP_DIR/gf" \
  Graphics/dll/vkd3d-proton/x86_64-windows/d3d12.dll \
  Graphics/dll/vkd3d-proton/x86_64-windows/d3d12core.dll \
  Graphics/dll/vkd3d-proton/x86_64-windows/dxgi.dll \
  Graphics/dll/dxvk/x86_64-windows/d3d9.dll \
  Graphics/dll/dxvk/x86_64-windows/d3d10core.dll \
  Graphics/dll/dxvk/x86_64-windows/d3d11.dll \
  Graphics/dll/dxvk/x86_64-windows/dxgi.dll \
  Graphics/dll/dxvk/i386-windows/d3d9.dll \
  Graphics/dll/dxvk/i386-windows/d3d10core.dll \
  Graphics/dll/dxvk/i386-windows/d3d11.dll \
  Graphics/dll/dxvk/i386-windows/dxgi.dll

rv_hash() { # runtime/wine-relative path
  local f="$TMP_DIR/rt/runtime/wine/$1"
  [ -e "$f" ] || { echo "error: $RUNTIME_ARCHIVE missing $1" >&2; exit 1; }
  shasum -a 256 "$f" | awk '{print $1}'
}
gf_hash() { # archive-root-relative path (relative to the Graphics/dll root)
  local f="$TMP_DIR/gf/Graphics/dll/$1"
  [ -e "$f" ] || { echo "error: $GRAPHICS_ARCHIVE missing $1" >&2; exit 1; }
  shasum -a 256 "$f" | awk '{print $1}'
}

MVK_LIB="$(rv_hash "lib/wine/x86_64-unix/libMoltenVK.dylib")"
MVK_LIB_SO="$(rv_hash "lib/wine/x86_64-unix/libMoltenVK.1.dylib")"
MVK_LANE_LIB="$(rv_hash "lib/moltenvk-vkmt/libMoltenVK.dylib")"
MVK_LANE_LIB_SO="$(rv_hash "lib/moltenvk-vkmt/libMoltenVK.1.dylib")"
MVK_LANE_ICD="$(rv_hash "lib/moltenvk-vkmt/MoltenVK_icd.json")"
MVK_RUNTIME_ICD="$(rv_hash "etc/vulkan/icd.d/MoltenVK_icd.json")"

VKD3D_D3D12="$(gf_hash "vkd3d-proton/x86_64-windows/d3d12.dll")"
VKD3D_D3D12CORE="$(gf_hash "vkd3d-proton/x86_64-windows/d3d12core.dll")"
VKD3D_DXGI="$(gf_hash "vkd3d-proton/x86_64-windows/dxgi.dll")"

DXVK_X64_D3D9="$(gf_hash "dxvk/x86_64-windows/d3d9.dll")"
DXVK_X64_D3D10CORE="$(gf_hash "dxvk/x86_64-windows/d3d10core.dll")"
DXVK_X64_D3D11="$(gf_hash "dxvk/x86_64-windows/d3d11.dll")"
DXVK_X64_DXGI="$(gf_hash "dxvk/x86_64-windows/dxgi.dll")"
DXVK_I386_D3D9="$(gf_hash "dxvk/i386-windows/d3d9.dll")"
DXVK_I386_D3D10CORE="$(gf_hash "dxvk/i386-windows/d3d10core.dll")"
DXVK_I386_D3D11="$(gf_hash "dxvk/i386-windows/d3d11.dll")"
DXVK_I386_DXGI="$(gf_hash "dxvk/i386-windows/dxgi.dll")"

UTILS="$ROOT_DIR/tools/bundles"

write_tsv() {
  local out="$1" header="$2"
  shift 2
  local tmp="$TMP_DIR/$(basename "$out").tmp"
  {
    printf '# Regenerated %s by %s — do not edit by hand.\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${0##*/}"
    printf '%s\n' "$header"
    printf 'path\tsha256\n'
    while [ "$#" -gt 0 ]; do
      printf '%s\t%s\n' "$1" "$2"
      shift 2
    done
  } > "$tmp"
  if [ "$CHECK" -eq 1 ]; then
    # Compare data rows only; the regeneration-timestamp header legitimately
    # changes on every run.
    if ! diff -q <(grep -v '^#' "$out" 2>/dev/null || true) <(grep -v '^#' "$tmp") >/dev/null 2>&1; then
      echo "DRIFT: $out differs from current bundles" >&2
      diff -u <(grep -v '^#' "$out") <(grep -v '^#' "$tmp") | tail -25 >&2 || true
      exit 1
    fi
    echo "OK: $out matches current bundles"
  else
    mv "$tmp" "$out"
    echo "wrote $out"
  fi
}

write_tsv "$UTILS/moltenvk-runtime-hashes.tsv" \
  "# Paths are relative to the runtime/wine root." \
  "lib/wine/x86_64-unix/libMoltenVK.dylib" "$MVK_LIB" \
  "lib/wine/x86_64-unix/libMoltenVK.1.dylib" "$MVK_LIB_SO" \
  "lib/moltenvk-vkmt/libMoltenVK.dylib" "$MVK_LANE_LIB" \
  "lib/moltenvk-vkmt/libMoltenVK.1.dylib" "$MVK_LANE_LIB_SO" \
  "lib/moltenvk-vkmt/MoltenVK_icd.json" "$MVK_LANE_ICD" \
  "etc/vulkan/icd.d/MoltenVK_icd.json" "$MVK_RUNTIME_ICD"

write_tsv "$UTILS/vkd3d-proton-runtime-hashes.tsv" \
  "# Paths are relative to the vkd3d-proton runtime lane root." \
  "x86_64-windows/d3d12.dll" "$VKD3D_D3D12" \
  "x86_64-windows/d3d12core.dll" "$VKD3D_D3D12CORE" \
  "x86_64-windows/dxgi.dll" "$VKD3D_DXGI"

write_tsv "$UTILS/dxvk-runtime-hashes.tsv" \
  "# Paths are relative to the dxvk runtime lane root." \
  "x86_64-windows/d3d9.dll" "$DXVK_X64_D3D9" \
  "x86_64-windows/d3d10core.dll" "$DXVK_X64_D3D10CORE" \
  "x86_64-windows/d3d11.dll" "$DXVK_X64_D3D11" \
  "x86_64-windows/dxgi.dll" "$DXVK_X64_DXGI" \
  "i386-windows/d3d9.dll" "$DXVK_I386_D3D9" \
  "i386-windows/d3d10core.dll" "$DXVK_I386_D3D10CORE" \
  "i386-windows/d3d11.dll" "$DXVK_I386_D3D11" \
  "i386-windows/dxgi.dll" "$DXVK_I386_DXGI"

[ "$CHECK" -eq 1 ] && exit 0

if [ "$SYNC_RUST" -eq 1 ]; then
  INSTALLER="$ROOT_DIR/app/src-rust/src/installer.rs"
  [ -f "$INSTALLER" ] || { echo "missing installer.rs: $INSTALLER" >&2; exit 2; }

  # Shell vars are exported so the (quoted-heredoc) python step can read them.
  export MVK_LIB MVK_LIB_SO MVK_LANE_LIB MVK_LANE_LIB_SO MVK_LANE_ICD MVK_RUNTIME_ICD
  export VKD3D_D3D12 VKD3D_D3D12CORE VKD3D_DXGI
  export DXVK_X64_D3D9 DXVK_X64_D3D10CORE DXVK_X64_D3D11 DXVK_X64_DXGI
  export DXVK_I386_D3D9 DXVK_I386_D3D10CORE DXVK_I386_D3D11 DXVK_I386_DXGI

  python3 - "$INSTALLER" <<'PY'
import os, re, sys
path = sys.argv[1]
E = os.environ
hashes = {
    "VKD3D_MOLTENVK_BUNDLE_LIBRARY_SHA256":     E["MVK_LIB"],
    "VKD3D_MOLTENVK_BUNDLE_LANE_ICD_SHA256":    E["MVK_LANE_ICD"],
    "VKD3D_MOLTENVK_BUNDLE_RUNTIME_ICD_SHA256": E["MVK_RUNTIME_ICD"],
    "vkd3d-proton": {
        "x86_64-windows/d3d12.dll":     E["VKD3D_D3D12"],
        "x86_64-windows/d3d12core.dll": E["VKD3D_D3D12CORE"],
        "x86_64-windows/dxgi.dll":      E["VKD3D_DXGI"],
    },
    "dxvk": {
        "x86_64-windows/d3d9.dll":      E["DXVK_X64_D3D9"],
        "x86_64-windows/d3d10core.dll": E["DXVK_X64_D3D10CORE"],
        "x86_64-windows/d3d11.dll":     E["DXVK_X64_D3D11"],
        "x86_64-windows/dxgi.dll":      E["DXVK_X64_DXGI"],
        "i386-windows/d3d9.dll":        E["DXVK_I386_D3D9"],
        "i386-windows/d3d10core.dll":   E["DXVK_I386_D3D10CORE"],
        "i386-windows/d3d11.dll":       E["DXVK_I386_D3D11"],
        "i386-windows/dxgi.dll":        E["DXVK_I386_DXGI"],
    },
}

src = open(path).read()
orig = src
HASH_RE = re.compile(r'[0-9a-f]{64}')

# MoltenVK constants are named; patch each exact declaration.
for name, value in hashes.items():
    if name == "vkd3d-proton" or name == "dxvk":
        continue
    pat = re.compile(r'(' + re.escape(name) + r': &str =)' + r'(?:\s*)"[0-9a-f]{64}"')
    new, n = pat.subn(lambda m: m.group(1) + f'"{value}"', src)
    if n != 1:
        raise SystemExit(f"constant {name}: expected 1 match, got {n}")
    src = new

# Lane PE tables: patch only inside the #[cfg(not(test))] block of the
# matching constant (the dxmt/dxmt-m12 tables also pin d3d12.dll, and the
# #[cfg(test)] tables pin the test-fixture hash; neither may be touched).
TEST_HASH = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
for const_name, lane in (("VKD3D_REQUIRED_PE", "vkd3d-proton"), ("DXVK_REQUIRED_PE", "dxvk")):
    m = re.search(
        r'^const ' + re.escape(const_name) + r': &\[\(&str, &str\)\] = &\[\n(.*?)\n\];',
        src, re.M | re.S,
    )
    if not m:
        raise SystemExit(f"{const_name} block not found")
    start, end = m.start(1), m.end(1)
    block = src[start:end]
    for rel, value in hashes[lane].items():
        pat = re.compile(r'\("' + re.escape(rel) + r'", "[0-9a-f]{64}"\)')
        def repl(mm, value=value):
            old_hash = HASH_RE.search(mm.group(0)).group(0)
            return mm.group(0) if old_hash == TEST_HASH else re.sub(HASH_RE, value, mm.group(0))
        patched, n = pat.subn(repl, block)
        if n == 0:
            raise SystemExit(f"{const_name}: no pin for {rel}")
        block = patched
    src = src[:start] + block + src[end:]

if src != orig:
    open(path, "w").write(src)
    print(f"updated {path}")
else:
    print(f"no changes needed in {path}")
PY

  if command -v cargo >/dev/null 2>&1 && [ -f "$ROOT_DIR/app/src-rust/Cargo.toml" ]; then
    cargo fmt --manifest-path "$ROOT_DIR/app/src-rust/Cargo.toml"
  fi
fi

echo "done"