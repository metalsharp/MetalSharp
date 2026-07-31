#!/bin/bash
# Required Notice: Copyright (c) 2026 MetalSharp. Commercial licensing: averyfelts@aol.com
set -euo pipefail

REPO="${METALSHARP_RUNTIME_REPO:-metalsharp/MetalSharp}"
TAG="${METALSHARP_RUNTIME_TAG:-v0.60.0-dependency-bundles}"
ARCHIVE_NAME="MetalSharp-Wine-Runtime-COMPLETE-all-arch-2026-07-31.tar.zst"
ARCHIVE_SHA256="93a456a40a7bf0ad2fecace5c01c58a366f85cc2901f6f8780c056c9e3b256ee"
PACKAGE_ROOT="MetalSharp-Wine-Runtime-COMPLETE-all-arch-2026-07-31"
PART_PREFIX="${ARCHIVE_NAME}.part"
PART01_SHA256="e3ff07bafe27fbf0f4fd7f761e962e7b3203e7e730c82d5d3e26fd5062bb0568"
PART02_SHA256="5e3c4e96898daa954fbf629819f760aadc3c73481c6200b1ade5e577131aa133"
PART03_SHA256="cf79389d77f0bd3002894f704948808510860306e0781b556ab91d233290a067"
PART04_SHA256="cc33cc12386793f47c1bc090f1d55e89c5f6971160ce4c3cbb7372c099b1232b"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CACHE_DIR="${METALSHARP_RUNTIME_CACHE:-$HOME/Library/Caches/MetalSharp/runtime-bundles/$TAG}"
TARGET="${METALSHARP_RUNTIME_TARGET:-$HOME/.metalsharp/runtime}"
BUNDLE_DIR=""
ARCHIVE_OVERRIDE=""
LOCAL_ONLY=0
PREPARE_ONLY=0
REPLACE=0
BACKUP_OLD=1
STAGE_DIR=""

usage() {
  cat <<'USAGE'
Usage: install-metalsharp-wine-runtime.sh [options]

Finds or downloads the four MetalSharp Wine runtime parts, verifies them,
reassembles and validates the tar.zst, prepares MetalSharp launch wrappers,
and installs the complete runtime transactionally.

Options:
  --archive PATH       Use an already-reassembled runtime archive.
  --bundle-dir DIR     Look for the archive or all four parts in DIR first.
  --cache-dir DIR      Download/reassembly cache directory.
  --target DIR         Install root (default: ~/.metalsharp/runtime).
  --local-only         Never use the network; fail if assets are not local.
  --prepare-only       Verify/reassemble the archive without extracting it.
  --replace            Replace an existing target after validation.
  --discard-backup     With --replace, delete the prior runtime after install.
  -h, --help           Show this help.

Without --replace, an existing runtime is preserved. Wine prefixes, Steam,
bottles, saves, and shader caches live outside the target and are never
removed by this installer.
USAGE
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

info() {
  echo "==> $*" >&2
}

cleanup() {
  if [ -n "$STAGE_DIR" ] && [ -d "$STAGE_DIR" ]; then
    find "$STAGE_DIR" -depth -delete
  fi
}
trap cleanup EXIT INT TERM

while [ "$#" -gt 0 ]; do
  case "$1" in
    --archive) [ "$#" -ge 2 ] || die "--archive requires a path"; ARCHIVE_OVERRIDE="$2"; shift 2 ;;
    --bundle-dir) [ "$#" -ge 2 ] || die "--bundle-dir requires a directory"; BUNDLE_DIR="$2"; shift 2 ;;
    --cache-dir) [ "$#" -ge 2 ] || die "--cache-dir requires a directory"; CACHE_DIR="$2"; shift 2 ;;
    --target) [ "$#" -ge 2 ] || die "--target requires a directory"; TARGET="$2"; shift 2 ;;
    --local-only) LOCAL_ONLY=1; shift ;;
    --prepare-only) PREPARE_ONLY=1; shift ;;
    --replace) REPLACE=1; shift ;;
    --discard-backup) BACKUP_OLD=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[ "$(uname -s)" = "Darwin" ] || die "this runtime targets macOS"
[ "$(uname -m)" = "arm64" ] || die "this runtime requires Apple Silicon (arm64)"
for tool in curl shasum tar awk sed find mktemp file grep python3; do
  command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool"
done

if command -v zstd >/dev/null 2>&1; then
  ZSTD="$(command -v zstd)"
elif [ -x /opt/homebrew/bin/zstd ]; then
  ZSTD=/opt/homebrew/bin/zstd
else
  die "zstd is required; install it with: brew install zstd"
fi

sha256_of() {
  shasum -a 256 "$1" | awk '{print $1}'
}

expected_part_sha() {
  case "$1" in
    01) echo "$PART01_SHA256" ;;
    02) echo "$PART02_SHA256" ;;
    03) echo "$PART03_SHA256" ;;
    04) echo "$PART04_SHA256" ;;
    *) die "invalid part number: $1" ;;
  esac
}

verify_file() {
  local path="$1" expected="$2" label="$3" actual
  [ -s "$path" ] || return 1
  actual="$(sha256_of "$path")"
  if [ "$actual" != "$expected" ]; then
    echo "REJECTED: $label has SHA-256 $actual" >&2
    return 1
  fi
  info "Verified $label"
}

verify_parts_dir() {
  local dir="$1" number path expected
  [ -d "$dir" ] || return 1
  for number in 01 02 03 04; do
    path="$dir/${PART_PREFIX}${number}"
    expected="$(expected_part_sha "$number")"
    verify_file "$path" "$expected" "part${number}" || return 1
  done
}

download_asset() {
  local name="$1" destination="$2" partial="${destination}.partial"
  [ "$LOCAL_ONLY" -eq 0 ] || die "missing local asset: $name"
  mkdir -p "$(dirname "$destination")"
  info "Downloading $name"
  if ! curl --fail --location --retry 4 --retry-delay 2 --continue-at - \
      --output "$partial" "https://github.com/$REPO/releases/download/$TAG/$name"; then
    find "$partial" -delete 2>/dev/null || true
    curl --fail --location --retry 4 --retry-delay 2 \
      --output "$partial" "https://github.com/$REPO/releases/download/$TAG/$name"
  fi
  mv "$partial" "$destination"
}

find_local_archive() {
  local candidate dir
  if [ -n "$ARCHIVE_OVERRIDE" ]; then
    verify_file "$ARCHIVE_OVERRIDE" "$ARCHIVE_SHA256" "$ARCHIVE_NAME" \
      || die "explicit archive failed verification: $ARCHIVE_OVERRIDE"
    echo "$ARCHIVE_OVERRIDE"
    return 0
  fi
  for dir in "$BUNDLE_DIR" "$PWD" "$SCRIPT_DIR" "$HOME/Downloads" "$HOME/Desktop" "$CACHE_DIR"; do
    [ -n "$dir" ] || continue
    candidate="$dir/$ARCHIVE_NAME"
    if verify_file "$candidate" "$ARCHIVE_SHA256" "$ARCHIVE_NAME"; then
      echo "$candidate"
      return 0
    fi
  done
  return 1
}

find_local_parts() {
  local dir
  for dir in "$BUNDLE_DIR" "$PWD" "$SCRIPT_DIR" "$HOME/Downloads" "$HOME/Desktop" "$CACHE_DIR"; do
    [ -n "$dir" ] || continue
    if verify_parts_dir "$dir"; then
      echo "$dir"
      return 0
    fi
  done
  return 1
}

prepare_archive() {
  local archive parts_dir number name path expected partial
  if archive="$(find_local_archive)"; then
    echo "$archive"
    return 0
  fi

  mkdir -p "$CACHE_DIR"
  if ! parts_dir="$(find_local_parts)"; then
    parts_dir="$CACHE_DIR"
    for number in 01 02 03 04; do
      name="${PART_PREFIX}${number}"
      path="$parts_dir/$name"
      expected="$(expected_part_sha "$number")"
      if ! verify_file "$path" "$expected" "part${number}"; then
        [ ! -e "$path" ] || find "$path" -delete
        download_asset "$name" "$path"
        verify_file "$path" "$expected" "part${number}" \
          || die "downloaded $name failed verification"
      fi
    done
  fi

  archive="$CACHE_DIR/$ARCHIVE_NAME"
  partial="${archive}.partial"
  info "Reassembling $ARCHIVE_NAME"
  cat "$parts_dir/${PART_PREFIX}01" "$parts_dir/${PART_PREFIX}02" \
      "$parts_dir/${PART_PREFIX}03" "$parts_dir/${PART_PREFIX}04" > "$partial"
  verify_file "$partial" "$ARCHIVE_SHA256" "$ARCHIVE_NAME" \
    || die "reassembled archive failed verification"
  mv "$partial" "$archive"
  echo "$archive"
}

validate_archive_layout() {
  local archive="$1" listing required
  info "Testing zstd stream"
  "$ZSTD" -q -t "$archive"
  listing="$("$ZSTD" -q -dc "$archive" | tar -tf -)"
  for required in \
    "$PACKAGE_ROOT/wine/build-ec/wine" \
    "$PACKAGE_ROOT/wine/build-ec/server/wineserver" \
    "$PACKAGE_ROOT/wine/wine-11.12/nls/locale.nls" \
    "$PACKAGE_ROOT/wine/wine-11.12/fonts/tahoma.ttf" \
    "$PACKAGE_ROOT/graphics/dxvk/i386/d3d9.dll" \
    "$PACKAGE_ROOT/graphics/vkd3d-proton/x86_64/d3d12.dll" \
    "$PACKAGE_ROOT/graphics/opengl-metal/metalsharp-opengl.dylib" \
    "$PACKAGE_ROOT/graphics/moltenvk/libMoltenVK.dylib" \
    "$PACKAGE_ROOT/providers/xtajit64-arm64ec-known-good.dll" \
    "$PACKAGE_ROOT/providers/xtajit-arm64-known-good.dll" \
    "$PACKAGE_ROOT/scripts/stage-runtime-providers.sh" \
    "$PACKAGE_ROOT/wine/build-ec/dlls/wow64/aarch64-windows/wow64.dll" \
    "$PACKAGE_ROOT/wine/build-ec/dlls/wow64win/aarch64-windows/wow64win.dll" \
    "$PACKAGE_ROOT/dependencies" \
    "$PACKAGE_ROOT/providers" \
    "$PACKAGE_ROOT/source/LICENSE-METALSHARP-POLYFORM-NONCOMMERCIAL.md" \
    "$PACKAGE_ROOT/metadata/SHA256SUMS"; do
    grep -Fqx "$required" <<< "$listing" \
      || grep -Fqx "$required/" <<< "$listing" \
      || die "archive is missing required path: $required"
  done
  if grep -Eq '(^|/)(encrypted-source/|[^/]*\.aes256$|[^/]*AES256\.key$)' <<< "$listing"; then
    die "archive unexpectedly contains encrypted source or an AES key"
  fi
  info "Archive layout verified"
}

write_launch_adapters() {
  local root="$1" bin="$1/wine/bin" launcher="$1/wine/bin/metalsharp-runtime-launcher"
  mkdir -p "$bin"
  cat > "$launcher" <<'LAUNCHER'
#!/bin/bash
set -euo pipefail
BIN_DIR="$(cd "$(dirname "$0")" && pwd)"
VKMT_RUNTIME_ROOT="$(cd "$BIN_DIR/../.." && pwd)"
export VKMT_RUNTIME_ROOT
export WINEBUILDDIR="$VKMT_RUNTIME_ROOT/wine/build-ec"
export WINEPREFIX="${WINEPREFIX:-$HOME/.metalsharp/prefix-steam}"
export FEX_TSOENABLED=0
export FEX_VECTORTSOENABLED=0
export FEX_MEMCPYSETTSOENABLED=0
. "$VKMT_RUNTIME_ROOT/scripts/vkmt-runtime-env.sh"
case "$(basename "$0")" in
  wineserver) exec "$WINEBUILDDIR/server/wineserver" "$@" ;;
  *) exec "$WINEBUILDDIR/wine" "$@" ;;
esac
LAUNCHER
  chmod 0755 "$launcher"
  ln -s metalsharp-runtime-launcher "$bin/wine"
  ln -s metalsharp-runtime-launcher "$bin/wine64"
  ln -s metalsharp-runtime-launcher "$bin/metalsharp-wine"
  ln -s metalsharp-runtime-launcher "$bin/wineserver"
}

repair_build_tree_links() {
  local root="$1" link destination mapped relative count=0
  while IFS= read -r link; do
    destination="$(readlink "$link")"
    case "$destination" in
      /Volumes/AverySSD/VKMT/*)
        mapped="$root/${destination#/Volumes/AverySSD/VKMT/}"
        [ -e "$mapped" ] || die "cannot relocate symlink target: $destination"
        relative="$(python3 - "$mapped" "$(dirname "$link")" <<'PY'
import os
import sys
print(os.path.relpath(sys.argv[1], sys.argv[2]))
PY
)"
        find "$link" -delete
        ln -s "$relative" "$link"
        count=$((count + 1))
        ;;
      /*) die "unmapped absolute symlink in runtime: $link -> $destination" ;;
    esac
  done < <(find "$root" -type l -print)
  info "Relocated $count absolute build-tree symlinks"
}

write_metalsharp_layout() {
  local root="$1" lib="$1/wine/lib"
  mkdir -p "$lib"
  ln -s ../build-ec/dxmt-v0.80 "$lib/dxmt"
  ln -s ../build-ec/dxmt-v0.80 "$lib/dxmt-m12"
  ln -s ../build-ec/dxmt-v0.80 "$lib/dxmt_m12"
  ln -s ../../graphics/dxvk "$lib/dxvk"
  ln -s ../../graphics/vkd3d-proton "$lib/vkd3d-proton"
  ln -s ../../graphics/opengl-metal "$lib/opengl-metal"
  ln -s ../../graphics/moltenvk "$lib/moltenvk"
}

install_archive() {
  local archive="$1" parent available_kb required_kb root backup marker
  parent="$(dirname "$TARGET")"
  marker="$TARGET/.metalsharp-runtime-install"
  if [ -f "$marker" ] && grep -Fq "archive_sha256=$ARCHIVE_SHA256" "$marker"; then
    info "This exact runtime is already installed at $TARGET"
    return 0
  fi
  if [ -e "$TARGET" ] && [ "$REPLACE" -ne 1 ]; then
    die "$TARGET already exists; rerun with --replace to preserve and replace it"
  fi

  mkdir -p "$parent"
  available_kb="$(df -Pk "$parent" | awk 'NR==2 {print $4}')"
  required_kb=19000000
  [ "$available_kb" -ge "$required_kb" ] \
    || die "installation needs at least 19,000,000 KiB free beside $TARGET"

  STAGE_DIR="$(mktemp -d "$parent/.metalsharp-runtime-stage.XXXXXX")"
  info "Extracting into transactional staging directory"
  "$ZSTD" -q -dc "$archive" | tar -xf - -C "$STAGE_DIR"
  root="$STAGE_DIR/$PACKAGE_ROOT"
  [ -d "$root" ] || die "extracted package root is missing"

  info "Verifying extracted payload hashes"
  (cd "$root" && shasum -a 256 -c metadata/SHA256SUMS >/dev/null)
  [ -x "$root/wine/build-ec/wine" ] || die "Wine launcher is not executable"
  [ -x "$root/wine/build-ec/server/wineserver" ] || die "wineserver is not executable"
  file "$root/wine/build-ec/wine" | grep -q 'arm64' || die "Wine host is not ARM64"
  file "$root/wine/build-ec/server/wineserver" | grep -q 'arm64' || die "wineserver host is not ARM64"
  repair_build_tree_links "$root"
  write_launch_adapters "$root"
  write_metalsharp_layout "$root"
  cat > "$root/.metalsharp-runtime-install" <<EOF
release=$TAG
archive=$ARCHIVE_NAME
archive_sha256=$ARCHIVE_SHA256
installed_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
no_tso=1
EOF

  backup="${parent}/.$(basename "$TARGET").backup.$(date -u '+%Y%m%dT%H%M%SZ')"
  if [ -e "$TARGET" ]; then
    [ ! -e "$backup" ] || die "backup path already exists: $backup"
    info "Preserving existing runtime at $backup"
    mv "$TARGET" "$backup"
  else
    backup=""
  fi

  if ! mv "$root" "$TARGET"; then
    [ -z "$backup" ] || mv "$backup" "$TARGET"
    die "failed to activate prepared runtime"
  fi
  find "$STAGE_DIR" -depth -delete
  STAGE_DIR=""

  if [ -n "$backup" ] && [ "$BACKUP_OLD" -eq 0 ]; then
    info "Deleting replaced runtime backup after successful activation"
    find "$backup" -depth -delete
    backup=""
  fi
  info "Installed MetalSharp Wine runtime at $TARGET"
  info "Wine launcher: $TARGET/wine/bin/metalsharp-wine"
  [ -z "$backup" ] || info "Previous runtime backup: $backup"
}

ARCHIVE="$(prepare_archive)"
verify_file "$ARCHIVE" "$ARCHIVE_SHA256" "$ARCHIVE_NAME" \
  || die "prepared archive failed final verification"
validate_archive_layout "$ARCHIVE"

if [ "$PREPARE_ONLY" -eq 1 ]; then
  info "Prepared archive: $ARCHIVE"
  exit 0
fi

install_archive "$ARCHIVE"
