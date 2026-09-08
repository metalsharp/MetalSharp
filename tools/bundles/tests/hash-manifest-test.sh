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
