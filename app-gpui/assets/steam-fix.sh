#!/usr/bin/env bash
set -euo pipefail

MS_HOME="${METALSHARP_HOME:-$HOME/.metalsharp}"
CEF_ROOT="$MS_HOME/prefix-steam/drive_c/Program Files (x86)/Steam/bin/cef"
URL='https://github.com/metalsharp/MetalSharp/releases/download/bundles/metalsharp-steam.tar.zst'
ARCHIVE="$MS_HOME/cache/bundles/metalsharp-steam.tar.zst"
DOWNLOAD="$ARCHIVE.download"
WRAPPER_SHA='f46a1e8c39c850ba22861f63559f13b4f68557acf04a92e6d1b899769b2ea1f9'
MAX=100000
tmp=''

cleanup() {
  [ -z "$tmp" ] || rm -rf "$tmp"
  rm -f "$DOWNLOAD"
}
trap cleanup EXIT

[ -d "$CEF_ROOT" ] || {
  echo "MetalSharp Steam CEF directory not found: $CEF_ROOT" >&2
  exit 1
}
command -v unzstd >/dev/null || {
  echo "unzstd not found; install zstd first." >&2
  exit 1
}

mkdir -p "${ARCHIVE%/*}"
curl --fail --location --retry 3 --retry-delay 1 \
  --silent --show-error -o "$DOWNLOAD" "$URL"
mv -f "$DOWNLOAD" "$ARCHIVE"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-steam-fix.XXXXXX")"
tar --use-compress-program=unzstd -xf "$ARCHIVE" -C "$tmp"

wrapper="$tmp/steam/steamwebhelper.exe"
printf '%s  %s\n' "$WRAPPER_SHA" "$wrapper" | shasum -a 256 -c - >/dev/null

count=0
for cef_dir in "$CEF_ROOT"/cef.*; do
  [ -d "$cef_dir" ] || continue

  original="$cef_dir/steamwebhelper.exe"
  real="$cef_dir/steamwebhelper_real.exe"
  marker="$cef_dir/.ms_wrapper_deployed"

  [ -e "$original" ] || [ -e "$real" ] || continue

  original_size=0
  [ -f "$original" ] && original_size=$(wc -c < "$original" | tr -d '[:space:]')

  # A full-size steamwebhelper.exe is Steam's own, freshly installed by a
  # client update: it always becomes steamwebhelper_real.exe, never deleted.
  if [ "$original_size" -gt "$MAX" ]; then
    rm -f "$real"
    mv "$original" "$real"
  fi

  cp -p "$wrapper" "$original.tmp.$$"
  mv -f "$original.tmp.$$" "$original"
  printf deployed > "$marker"

  [ -f "$real" ] && [ "$(wc -c < "$real" | tr -d '[:space:]')" -gt "$MAX" ] || {
    echo "No original Steam webhelper was available in $cef_dir" >&2
    exit 1
  }

  count=$((count + 1))
done

[ "$count" -gt 0 ] || {
  echo "No Steam CEF installations found under $CEF_ROOT" >&2
  exit 1
}

# Steam's client manifests (package/<name>.installed) list each CEF directory's
# webhelper as "bin\cef\<dir>\steamwebhelper.exe,<size>;<mtime>;<crc32>". Older
# MetalSharp deleted the webhelper a Steam update installed and kept the old
# one as steamwebhelper_real.exe, which hangs Steam's UI. Restore it from
# Steam's own package/*webhelper*.zip.vz.* download.
PACKAGE_DIR="${CEF_ROOT%/bin/cef}/package"

# VZ is "VZa", a 4-byte timestamp, 5 LZMA properties, raw LZMA data, then
# CRC32, uncompressed size and "zv"; as an .lzma stream bsdtar reads the zip.
vz_extract() {
  local vz="$1" entry="$2" out="$3" length lzma="$tmp/webhelper.lzma"
  length=$(wc -c < "$vz" | tr -d '[:space:]')
  [ "$length" -gt 22 ] && [ "$(head -c 3 "$vz")" = VZa ] || return 1
  {
    tail -c +8 "$vz" | head -c 5
    tail -c 6 "$vz" | head -c 4
    printf '\0\0\0\0'
    tail -c +13 "$vz" | head -c $((length - 22))
  } > "$lzma"
  /usr/bin/tar -xOf "$lzma" "$entry" > "$out"
}

stale=0
for manifest in "$PACKAGE_DIR"/*.installed; do
  [ -f "$manifest" ] || continue
  while IFS=, read -r entry fields || [ -n "$entry" ]; do
    # shellcheck disable=SC1003 # backslashes are literal Windows path separators
    case "$entry" in 'bin\cef\'*'\steamwebhelper.exe') ;; *) continue ;; esac
    # shellcheck disable=SC1003
    cef_name=${entry#'bin\cef\'}
    cef_name=${cef_name%'\steamwebhelper.exe'}
    case "$cef_name" in '' | *\\*) continue ;; esac
    fields=${fields%$'\r'}
    want_size=${fields%%;*}
    want_crc=${fields##*;}
    real="$CEF_ROOT/$cef_name/steamwebhelper_real.exe"
    [ -f "$CEF_ROOT/$cef_name/steamwebhelper.exe" ] || continue
    read -r have_crc have_size _ < <(cksum -o 3 "$real" 2>/dev/null || echo "0 0")
    [ "$have_crc" = "$want_crc" ] && [ "$have_size" = "$want_size" ] && continue
    restored=0
    staged="$real.ms-restore"
    for vz in "$PACKAGE_DIR"/*webhelper*.zip.vz.*; do
      [ -f "$vz" ] || continue
      if vz_extract "$vz" "bin/cef/$cef_name/steamwebhelper.exe" "$staged"; then
        read -r have_crc have_size _ < <(cksum -o 3 "$staged")
        if [ "$have_crc" = "$want_crc" ] && [ "$have_size" = "$want_size" ]; then
          chmod 755 "$staged"
          mv -f "$staged" "$real"
          restored=1
          break
        fi
      fi
      rm -f "$staged"
    done
    if [ "$restored" = 1 ]; then
      echo "Restored Steam's updated webhelper for $cef_name."
    else
      echo "Steam's webhelper for $cef_name is out of date and its update package was not found." >&2
      stale=1
    fi
  done < "$manifest"
done
[ "$stale" = 0 ] || exit 1

echo "Deployed MetalSharp Steam webhelper wrapper to $count CEF directories."
