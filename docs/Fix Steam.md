### Did Steam break for you after updating? 

**_You can run the following command to fix it:_**

___

bash <<'METALSHARP_STEAM_FIX'
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
  real_size=0
  [ -f "$original" ] && original_size=$(wc -c < "$original" | tr -d '[:space:]')
  [ -f "$real" ] && real_size=$(wc -c < "$real" | tr -d '[:space:]')

  if [ "$original_size" -gt "$MAX" ]; then
    if [ "$real_size" -le "$MAX" ]; then
      rm -f "$real"
      mv "$original" "$real"
    else
      rm -f "$original"
    fi
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

echo "Deployed MetalSharp Steam webhelper wrapper to $count CEF directories."
METALSHARP_STEAM_FIX
