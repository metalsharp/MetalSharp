# Building MetalSharp Wine
**Updated:** 2026-10-05

Builds the **Wine 11.17** runtime: x86_64 Unix host (runs under Rosetta) with i386 + x86_64 PE (WoW64). Build into a separate directory, never over `~/.metalsharp/runtime` or a live Steam prefix.

## Source

You need the **prepared MetalSharp-patched Wine 11.17 tree** (get it from the maintainer; no public URL yet). Stock Wine lacks MetalSharp's D3DMetal/DXMT loaders, macOS presentation, MSYNC (client and server must come from the same build), WoW64 bootstrap, launcher, and x87 changes. Apply `tools/bundles/wine/0001-x87sidecar-cooperative.patch` once before configuring.

D3DMetal, DXMT (v0.80), DXVK/VKD3D-Proton, and MoltenVK payloads are separate; `make install` does not provide them. D3DMetal launches need `D3DMETAL_RUNTIME_DIR` (payload root) and `D3DMETAL_FRAMEWORK_PATH` (`external/D3DMetal.framework/D3DMetal`). Preserve upstream copyright and license notices (see `LICENSES/WINEFORGE-NOTICES.md`).

## Tools

```bash
xcode-select --install
softwareupdate --install-rosetta --agree-to-license
brew install autoconf automake bison flex pkgconf llvm mingw-w64 python
command -v i686-w64-mingw32-gcc x86_64-w64-mingw32-gcc
```

## x86_64 Dependencies

Every library linked into Wine's Unix side needs an **x86_64 slice**; arm64 Homebrew libraries don't count. Provide a prefix `$DEPS` (`include/`, `lib/`, `bin/`, `lib/pkgconfig/`) with: FreeType, GnuTLS, FFmpeg, GStreamer + GLib, SDL2, Samba NetAPI (`netapi.h`, `libnetapi.dylib`, `ntlm_auth`), Vulkan loader + MoltenVK, OpenCL headers, and gettext. Check with `lipo -archs "$DEPS/lib/libgnutls.dylib"`.

## Configure and Build

Use absolute paths without spaces.

```bash
export ROOT="/Volumes/BuildSSD/MetalSharpWine"
export SRC="$ROOT/sources/wine-11.17-metalsharp"
export DEPS="$ROOT/deps/x86_64"
export BUILD="$ROOT/build/wine-x86_64"
export PREFIX="$ROOT/install/wine"
export X87_WINE_PATCH="/path/to/MetalSharp/tools/bundles/wine/0001-x87sidecar-cooperative.patch"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"

export PATH="$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$(brew --prefix llvm)/bin:$DEPS/bin:$PATH"
export CC="clang -arch x86_64" CXX="clang++ -arch x86_64" CPP="clang -arch x86_64 -E" OBJC="clang -arch x86_64"
export CFLAGS="-O2 -isysroot $SDKROOT" CXXFLAGS="-O2 -isysroot $SDKROOT" OBJCFLAGS="-O2 -isysroot $SDKROOT"
export CPPFLAGS="-I$DEPS/include"
export LDFLAGS="-arch x86_64 -isysroot $SDKROOT -L$DEPS/lib -Wl,-rpath,$DEPS/lib"
export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig:$DEPS/share/pkgconfig"; unset PKG_CONFIG_PATH
export i386_CC="$(command -v i686-w64-mingw32-gcc)" x86_64_CC="$(command -v x86_64-w64-mingw32-gcc)"
export NETAPI_CFLAGS="-I$DEPS/include" NETAPI_LIBS="-L$DEPS/lib -lnetapi"
export OPENCL_CFLAGS="-I$DEPS/include" OPENCL_LIBS="-framework OpenCL"

mkdir -p "$BUILD" "$PREFIX" "$ROOT/logs"
git -C "$SRC" apply --check "$X87_WINE_PATCH" && git -C "$SRC" apply "$X87_WINE_PATCH"
(cd "$SRC" && autoreconf -fiv)
cd "$BUILD"
set -o pipefail
"$SRC/configure" \
  --build=x86_64-apple-darwin --enable-archs=i386,x86_64 --prefix="$PREFIX" \
  --disable-tests --disable-winemenubuilder \
  --with-coreaudio --with-cups --with-ffmpeg --with-freetype \
  --with-gettext --with-gnutls --with-gstreamer --with-mingw \
  --with-netapi --with-opencl --with-opengl --with-pcap \
  --with-pthread --with-sdl --with-vulkan \
  --without-alsa --without-capi --without-dbus --without-gphoto \
  --without-inotify --without-krb5 --without-oss --without-pulse \
  --without-sane --without-udev --without-usb --without-v4l2 \
  --without-wayland --without-x \
  2>&1 | tee "$ROOT/logs/configure.log"
make -j"$(sysctl -n hw.ncpu)" 2>&1 | tee "$ROOT/logs/build.log"
make install 2>&1 | tee "$ROOT/logs/install.log"
```

Use `--enable-archs=i386,x86_64`, not `--enable-win64`. If configure can't find a dependency, set its `*_CFLAGS`/`*_LIBS` to the x86_64 prefix; don't force cache results. Use a fresh build directory when changing the source, arch, or dependencies.

## x87 Sidecar

```bash
export X87SIDECAR_VERSION="v1.7.0"
export X87SIDECAR_SHA256="b768336e0ad556807156cecd533423285c8ec654f4fdb9c0623124d8b12c0865"
ARCHIVE="$ROOT/downloads/x87sidecar-$X87SIDECAR_VERSION.tar.xz"
OUT="$ROOT/downloads/x87sidecar-$X87SIDECAR_VERSION"
mkdir -p "$OUT"
curl --fail --location --proto '=https' --tlsv1.2 \
  "https://github.com/athei/x87sidecar/releases/download/$X87SIDECAR_VERSION/x87sidecar.tar.xz" -o "$ARCHIVE"
printf '%s  %s\n' "$X87SIDECAR_SHA256" "$ARCHIVE" | shasum -a 256 -c -
tar -xJf "$ARCHIVE" -C "$OUT"
xattr -d com.apple.quarantine "$OUT/x87sidecar" 2>/dev/null || true
install -m 0755 "$OUT/x87sidecar" "$PREFIX/bin/x87sidecar"
"$PREFIX/bin/x87sidecar" --probe
```

The sidecar is used only for i386 images when the launcher sets `ROSETTA_X87_PATH`. Keep it at the same version as the patch.

## Validate

```bash
export DYLD_FALLBACK_LIBRARY_PATH="$DEPS/lib:$PREFIX/lib:$PREFIX/lib/wine/x86_64-unix"
"$PREFIX/bin/wine" --version                       # wine-11.17
file "$PREFIX/lib/wine/x86_64-windows/ntdll.dll" "$PREFIX/lib/wine/i386-windows/ntdll.dll"

export WINEPREFIX="$(mktemp -d "$ROOT/smoke-prefix.XXXXXX")"   # never ~/.metalsharp/prefix-steam
export WINEARCH=wow64 WINESERVER="$PREFIX/bin/wineserver" WINEMSYNC=1
"$PREFIX/bin/wine" wineboot -u
"$PREFIX/bin/wine" cmd /c ver
"$PREFIX/bin/wineserver" -w
```

Never run `wineserver -k` against a live Steam prefix. `make install` output is a development install, not a relocatable bundle: audit load paths, include the host libraries it needs, and re-sign any modified Mach-O before shipping.
