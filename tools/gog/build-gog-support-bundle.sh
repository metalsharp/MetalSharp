#!/usr/bin/env bash
set -euo pipefail

GOGDL_TAG="v1.2.2"
GOGDL_COMMIT="4928e46d1fc4e8f230fe45de277acb8358cbdd69"
XDELTA_COMMIT="0525275fe4b553a10f38e455d30c60dc6ed9b45d"
PYINSTALLER_VERSION="6.16.0"
URLLIB3_VERSION="1.26.20"
PACKAGE_ROOT="MetalSharp-GOG-Support-arm64-1.2.2"
OUTPUT_DIR="${1:-$PWD}"

fail() {
  echo "ERROR: $*" >&2
  exit 1
}

[ "$(uname -s)" = "Darwin" ] || fail "GOG support must be built on macOS"
[ "$(uname -m)" = "arm64" ] || fail "GOG support must be built natively on Apple Silicon"
for tool in git shasum tar zstd file codesign; do
  command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
done
[ -x /usr/bin/python3 ] || fail "/usr/bin/python3 is required"
mkdir -p "$OUTPUT_DIR"

BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-gog-support.XXXXXX")"
cleanup() {
  find "$BUILD_DIR" -depth -delete 2>/dev/null || true
}
trap cleanup EXIT INT TERM

SOURCE="$BUILD_DIR/heroic-gogdl"
VENV="$BUILD_DIR/venv"
STAGE="$BUILD_DIR/$PACKAGE_ROOT"

git clone --quiet --depth 1 --branch "$GOGDL_TAG" --recurse-submodules \
  https://github.com/Heroic-Games-Launcher/heroic-gogdl.git "$SOURCE"
[ "$(git -C "$SOURCE" rev-parse HEAD)" = "$GOGDL_COMMIT" ] || fail "unexpected heroic-gogdl commit"
[ "$(git -C "$SOURCE/xdelta3" rev-parse HEAD)" = "$XDELTA_COMMIT" ] || fail "unexpected xdelta3 commit"

/usr/bin/python3 -m venv "$VENV"
"$VENV/bin/python" -m pip install --disable-pip-version-check --quiet --upgrade pip setuptools wheel
"$VENV/bin/python" -m pip install --disable-pip-version-check --quiet \
  "pyinstaller==$PYINSTALLER_VERSION" "urllib3==$URLLIB3_VERSION" "$SOURCE"
"$VENV/bin/pyinstaller" --clean --noconfirm --onefile --name gogdl \
  --distpath "$BUILD_DIR/dist" --workpath "$BUILD_DIR/work" --specpath "$BUILD_DIR" \
  "$SOURCE/gogdl/cli.py" >/dev/null

BINARY="$BUILD_DIR/dist/gogdl"
file "$BINARY" | grep -q 'Mach-O 64-bit executable arm64' || fail "gogdl is not thin ARM64 Mach-O"
codesign --verify --deep --strict "$BINARY"
[ "$("$BINARY" --version)" = "1.2.2" ] || fail "gogdl version probe failed"
"$VENV/bin/pyi-archive_viewer" -l "$BINARY" | grep -q 'gogdl_xdelta3.abi3.so' \
  || fail "gogdl xdelta extension is not embedded"

mkdir -p "$STAGE/integration/gog/bin" "$STAGE/integration/gog/licenses" "$STAGE/integration/gog/metadata"
cp "$BINARY" "$STAGE/integration/gog/bin/gogdl"
chmod 0755 "$STAGE/integration/gog/bin/gogdl"
cp "$SOURCE/LICENSE" "$STAGE/integration/gog/licenses/heroic-gogdl-GPL-3.0.txt"
cp "$SOURCE/README.md" "$STAGE/integration/gog/metadata/heroic-gogdl-README.md"
cat > "$STAGE/integration/gog/metadata/PROVENANCE.tsv" <<EOF
component\tversion\trevision\tlicense\tupstream
heroic-gogdl\t1.2.2\t$GOGDL_COMMIT\tGPL-3.0\thttps://github.com/Heroic-Games-Launcher/heroic-gogdl
xdelta3\t3.x\t$XDELTA_COMMIT\tApache-2.0\thttps://github.com/jmacd/xdelta
PyInstaller\t$PYINSTALLER_VERSION\tpypi\tGPL-2.0-or-later-with-bootloader-exception\thttps://pyinstaller.org/
requests\t2.32.5\tpypi\tApache-2.0\thttps://requests.readthedocs.io/
urllib3\t$URLLIB3_VERSION\tpypi\tMIT\thttps://urllib3.readthedocs.io/
EOF
(cd "$STAGE" && find . -type f ! -path './integration/gog/metadata/SHA256SUMS' -print0 \
  | sort -z | xargs -0 shasum -a 256 > integration/gog/metadata/SHA256SUMS)

ARCHIVE="$OUTPUT_DIR/$PACKAGE_ROOT.tar.zst"
tar -C "$BUILD_DIR" -cf - "$PACKAGE_ROOT" | zstd -T0 -19 -f -o "$ARCHIVE"
shasum -a 256 "$ARCHIVE" > "$ARCHIVE.sha256"
echo "Created $ARCHIVE"
