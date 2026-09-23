#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
# Load only the helper, not the verifier's command-line entry point.
eval "$(sed -n '/^verify_hash_manifest() {/,/^}/p' "$ROOT/tools/bundles/verify-bundles.sh")"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/input/assets"
printf 'payload\n' > "$tmp/input/assets/test.dll"
tar -cf "$tmp/test.tar" -C "$tmp/input" assets
zstd -q "$tmp/test.tar" -o "$tmp/test.tar.zst"
hash="$(shasum -a 256 "$tmp/input/assets/test.dll" | awk '{print $1}')"
printf 'path\tsha256\ntest.dll\t%s\n' "$hash" > "$tmp/hashes.tsv"
verify_hash_manifest "$tmp/test.tar.zst" TEST assets "$tmp/hashes.tsv"

# Simulate BSD tar returning early from a selected-file zstd stream, closing
# the decompressor pipe with EPIPE even though the requested member is present.
real_tar="$(command -v tar)"
mkdir -p "$tmp/mockbin"
cat > "$tmp/mockbin/tar" <<'MOCK_TAR'
#!/usr/bin/env bash
for arg in "$@"; do
  if [ "$arg" = "--use-compress-program=unzstd" ]; then
    printf 'zstd: stdout: Broken pipe\n' >&2
    exit 1
  fi
done
exec "$REAL_TAR" "$@"
MOCK_TAR
chmod +x "$tmp/mockbin/tar"
PATH="$tmp/mockbin:$PATH" REAL_TAR="$real_tar" \
  verify_hash_manifest "$tmp/test.tar.zst" TEST assets "$tmp/hashes.tsv"
printf 'path\tsha256\ntest.dll\tbad\n' > "$tmp/hashes.tsv"
if verify_hash_manifest "$tmp/test.tar.zst" TEST assets "$tmp/hashes.tsv" 2>"$tmp/error"; then
  echo 'corrupt hash accepted' >&2; exit 1
fi
grep -q 'HASH MISMATCH' "$tmp/error"
printf 'path\tsha256\nmissing.dll\t%s\n' "$hash" > "$tmp/hashes.tsv"
if verify_hash_manifest "$tmp/test.tar.zst" TEST assets "$tmp/hashes.tsv" 2>"$tmp/error"; then
  echo 'missing member accepted' >&2; exit 1
fi
grep -q 'INVALID: unable to extract' "$tmp/error"
grep -qi 'missing.dll' "$tmp/error"
printf 'hash manifest regression tests passed\n'
