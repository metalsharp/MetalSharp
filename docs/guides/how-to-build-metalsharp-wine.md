# How to Build MetalSharp Wine

This guide builds MetalSharp's **Wine 11.17** runtime on macOS with an
**x86_64 Unix host** and **i386 + x86_64 Windows PE support**. On Apple Silicon,
the Unix host runs through Rosetta. This is the 64-bit-host WoW64 configuration;
it does not require a 32-bit macOS runtime.

Build into a separate directory. Do not build over the installed MetalSharp
runtime or use a live Steam prefix for build validation.

## Source and version

Use a **prepared MetalSharp-patched Wine 11.17 source tree**. A public URL and
immutable revision for this exact source snapshot are not yet provided. Obtain
the prepared tree from the maintainer before following these commands. A stock
Wine tree is not a substitute for MetalSharp's loader, graphics integration, and
synchronization changes.

The source directory must contain `configure.ac`, `dlls/`, `include/`, `server/`,
and `tools/`. Keep the source snapshot's checksum/revision with your build logs.
Do not substitute a different runtime solely because it reports Wine 11.17.

## Necessary tools

- An Apple Silicon Mac with Rosetta 2, or a compatible Intel Mac.
- Xcode Command Line Tools and a macOS SDK.
- Git, GNU Make, Autoconf, Automake, Bison, Flex, pkg-config, and Python 3.
- LLVM/Clang for x86_64 Mach-O host code.
- MinGW-w64 compilers for both i686 and x86_64 Windows PE code.
- Apple's `codesign`, `file`, `otool`, and `lipo` for validation.

Install the host tools with Homebrew:

```bash
xcode-select --install  # Skip if already installed.
brew install autoconf automake bison flex pkgconf llvm mingw-w64 python
```

On Apple Silicon, install Rosetta if it is not already available:

```bash
softwareupdate --install-rosetta --agree-to-license
```

Confirm the SDK and cross-compilers are available:

```bash
xcrun --sdk macosx --show-sdk-path
xcrun --find codesign
command -v i686-w64-mingw32-gcc
command -v x86_64-w64-mingw32-gcc
```

## Host dependencies

The compiler can run natively on Apple Silicon, but **every library linked into
Wine's Unix side must provide an x86_64 slice**. Installing an arm64 Homebrew
library does not satisfy this requirement. Use a prepared x86_64 dependency
prefix, or build these dependencies for x86_64 before configuring Wine. Headers
must match the installed libraries; do not mix unrelated versions.

| Feature | Required development files / runtime libraries |
| --- | --- |
| Fonts | FreeType |
| TLS | GnuTLS and its transitive libraries |
| Media | FFmpeg (`avformat`, `avcodec`, `avutil`) |
| GStreamer | GStreamer core, video, audio, tag and base; GLib/GObject/GIO and their dependencies |
| Controllers | SDL2-compatible headers and library |
| Windows authentication | Samba NetAPI (`netapi.h`, `libnetapi.dylib`) and `ntlm_auth` |
| Vulkan | Vulkan headers/loader and the compatible MoltenVK driver |
| OpenGL/EGL | macOS OpenGL support and any required EGL development files |
| OpenCL | OpenCL headers and the macOS OpenCL framework |
| Localization | gettext/libintl |
| System integration | macOS SDK CoreAudio, CUPS, pcap and pthread support |

The examples below expect these files in `$DEPS/include`, `$DEPS/lib`,
`$DEPS/bin`, and `$DEPS/lib/pkgconfig` (or `share/pkgconfig`). Dependency recipes
and binaries are separate prerequisites, not downloaded by these commands.
Check representative libraries before proceeding:

```bash
file "$DEPS/lib/libgnutls.dylib" "$DEPS/lib/libnetapi.dylib"
lipo -archs "$DEPS/lib/libgnutls.dylib"
```

Do not force successful configure-cache results to hide missing libraries.
Resolve missing headers, symbols, or architecture mismatches instead.

## Configure and build

Use absolute paths without spaces for the build/dependency directories. Set
`SRC` to the prepared source tree and `DEPS` to your x86_64 dependency prefix.
The example uses an external SSD; substitute an existing writable volume.

```bash
export ROOT="/Volumes/BuildSSD/MetalSharpWine"
export SRC="$ROOT/sources/wine-11.17-metalsharp"
export DEPS="$ROOT/deps/x86_64"
export BUILD="$ROOT/build/wine-x86_64"
export PREFIX="$ROOT/install/wine"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"

export PATH="$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$(brew --prefix llvm)/bin:$DEPS/bin:$PATH"
export CC="clang -arch x86_64"
export CXX="clang++ -arch x86_64"
export CPP="clang -arch x86_64 -E"
export OBJC="clang -arch x86_64"
export CFLAGS="-O2 -isysroot $SDKROOT"
export CXXFLAGS="$CFLAGS"
export OBJCFLAGS="$CFLAGS"
export CPPFLAGS="-I$DEPS/include"
export LDFLAGS="-arch x86_64 -isysroot $SDKROOT -L$DEPS/lib -Wl,-rpath,$DEPS/lib"
export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig:$DEPS/share/pkgconfig"
unset PKG_CONFIG_PATH
export i386_CC="$(command -v i686-w64-mingw32-gcc)"
export x86_64_CC="$(command -v x86_64-w64-mingw32-gcc)"
export NETAPI_CFLAGS="-I$DEPS/include"
export NETAPI_LIBS="-L$DEPS/lib -lnetapi"
export OPENCL_CFLAGS="-I$DEPS/include"
export OPENCL_LIBS="-framework OpenCL"

test -f "$SRC/configure.ac"
test -f "$DEPS/include/netapi.h"
test -f "$DEPS/lib/libnetapi.dylib"
test -x "$DEPS/bin/ntlm_auth"
mkdir -p "$BUILD" "$PREFIX" "$ROOT/logs"

(cd "$SRC" && autoreconf -fiv)
cd "$BUILD"
set -o pipefail
"$SRC/configure" \
  --build=x86_64-apple-darwin \
  --enable-archs=i386,x86_64 \
  --prefix="$PREFIX" \
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

Stop if configure or make fails. Inspect `config.log` and the configure summary
before treating the build as complete. If dependency discovery needs explicit
`GNUTLS_CFLAGS`/`GNUTLS_LIBS`, `FFMPEG_*`, `GSTREAMER_*`, or `SDL2_*` values,
point them at the matching x86_64 development prefix.

Do not replace `--enable-archs=i386,x86_64` with `--enable-win64`: that would not
build the intended dual-PE runtime. Use a fresh build directory when changing
source snapshots, architecture, or dependency versions.

## Validate without touching Steam

```bash
export DYLD_FALLBACK_LIBRARY_PATH="$DEPS/lib:$PREFIX/lib:$PREFIX/lib/wine/x86_64-unix"
"$PREFIX/bin/wine" --version
file "$PREFIX/bin/wine" "$PREFIX/bin/wineserver"
file "$PREFIX/lib/wine/x86_64-windows/ntdll.dll"
file "$PREFIX/lib/wine/i386-windows/ntdll.dll"
otool -L "$PREFIX/lib/wine/x86_64-unix/ntdll.so"

# A new prefix, never ~/.metalsharp/prefix-steam:
export WINEPREFIX="$(mktemp -d "$ROOT/smoke-prefix.XXXXXX")"
export WINEARCH=wow64
export WINESERVER="$PREFIX/bin/wineserver"
export WINEMSYNC=1
"$PREFIX/bin/wine" wineboot -u
"$PREFIX/bin/wine" cmd /c ver
"$PREFIX/bin/wineserver" -w
```

Expect `wine-11.17`, x86_64 Mach-O host binaries, and both x86_64 and i386 PE
DLLs. These commands are smoke checks, not a complete game-compatibility test.
MSYNC is selected at process/server startup; changing `WINEMSYNC` does not
reconfigure an already-running server. Never use `wineserver -k` against a live
Steam prefix to validate a build.

## Packaging is a separate step

`make install` creates a development installation. It does not produce a
relocatable MetalSharp runtime bundle. Audit dependency load paths and include
required redistributable host libraries before moving it to another machine.
Preserve applicable licenses and source notices. Sign final native binaries
after any Mach-O modifications and verify the resulting app signature.

DXMT, VKD3D-Proton/DXVK, MoltenVK routing metadata, and D3DMetal payloads are
separate graphics components; building Wine does not install them. Likewise,
Unity games commonly include their own managed assemblies and embedded Mono.
Do not copy arbitrary game DLLs into Wine to complete this build.
