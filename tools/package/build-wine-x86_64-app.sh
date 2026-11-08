#!/bin/bash
# Builds MetalSharp-Wine-x86_64.app: the regular build's x86_64 runtime (Wine 11.17 under Rosetta) as one signed
# app, as MetalSharp-Wine.app is for the FEX build. Its layout is the x86_64 install's:
#
#   Contents/MacOS, bin -> MacOS       runtime/wine/bin's Mach-O programs (wine, wineserver, tools)
#   Contents/lib, share, include       runtime/wine/{lib,share,include}; lib/dxmt is Mullion's lane
#   Contents/share/metalsharp/
#     wine-bin/                        runtime/wine/bin's scripts (metalsharp-wine, ...), which a facade installs
#     etc/                             runtime/wine/etc: dxmt.conf, mullion.conf, vkd3d.conf, dxvk.conf, vulkan/icd.d
#     runtime/<name>                   ~/.metalsharp/runtime/<name>: host, d3dmetal-gptk4-beta3, d3dmetal-gptk3, wfdxcompat,
#                                      wfdxcompat-agility, mono-x86, mono-arm64, dxvk-1.10.3, goldberg, shims, fnalibs,
#                                      fna-kickstart, faudio, steam-bridge, launchers, metalsharp-backend
#     vkd3d/{vkd3d-proton,dxvk}        ~/.metalsharp/vkd3d/...: the VKD3D lane, D3D12 to D3D8 on MoltenVK, whose
#                                      WINEDLLPATH is vkd3d-proton/x86_64-windows then dxvk/x86_64-windows
#     scripts/tools, shader-cache      ~/.metalsharp/scripts/tools, ~/.metalsharp/shader-cache
#     steam/                           the Steam installer and CEF wrapper (metalsharp-steam)
#     assets/                          the asset bundle's other trees (gptk, sdl3, xna, unity-mono, prebuilt-launchers)
#     graphics/{d3dmetal,external}     the graphics bundle's D3DMetal lane files
#     bundle-manifest.tsv              the bundles release's manifest
#   Contents/lib, lib/gstreamer-1.0,   GStreamer and FFmpeg (MEDIA: deps/prefix-x86_64 of MetalSharp Wine's
#     libexec/gstreamer-1.0            build-deps-x86_64.sh), which winegstreamer.so and winedmo.so link and the
#                                      bundles never carried
#
# from the bundles release (metalsharp/MetalSharp, tag bundles) with these upgrades:
#   lib/dxmt                Mullion (x86_64 and i386 PE, x86_64 unix side), in place of DXMT 0.80
#   vkd3d/vkd3d-proton      the VKD3D lane's D3D12 half: VKD3D-Proton-MacOS's release d3d12/d3d12core, with DXVK-MacOS's
#                           dxgi/d3d11/d3d10core (the lane's DXGI), and the NGX core and NVAPI (DLSS on MetalFX)
#   vkd3d/dxvk              its D3D11 to D3D8 half: DXVK-MacOS's release (x86_64 and i386), with the same NGX core
#                           and NVAPI
#   MoltenVK                VKD3D-Proton-MacOS's universal MoltenVK, Metal Shader Converter and dxbcconverter
#   d3dmetal-gptk4-beta3    D3DMetal from Game Porting Toolkit 4.0 beta 3, Apple-signed and kept so: the D3DMetal lane
#                           (with DLSS on MetalFX) on macOS 27 and later
#   d3dmetal-gptk3          D3DMetal from Game Porting Toolkit 3.0 (its evaluation environment's redist), Apple-signed and
#                           kept so: the D3DMetal lane below macOS 27, which the backend picks by the host's version
#
# Every Mach-O is signed with SIGN_IDENTITY (hardened runtime), inside out, except Apple-signed code (D3DMetal,
# libd3dshared, Metal Shader Converter), which keeps Apple's signature.
#
#   BUNDLES=<dir with the bundles release's .tar.zst> MULLION=<lib/dxmt> DXVK=<dxvk-macos-vX-x86_64 release dir>
#   DXVK_I386=<dxvk-macos-vX-i386 release dir> VKD3D=<vkd3d-proton-vX-x86_64 release dir> MOLTENVK=<moltenvk-vX-universal dir> NGX=<x86_64 nvngx/nvapi64 dir>
#   WINE_BUILD=<the x86_64 Wine's build tree> WINE_MODULES="ntdll win32u ..." (modules rebuilt there with MetalSharp
#   Wine's fixes, which replace the bundle's; server for the wineserver)
#   WINEMETALGL=<install-x86_64/winemetalgl> VCRUN_X64=<Microsoft's x64 VC++ runtime DLLs> DEPS_SRC=<deps/src-x86_64:deps/src> LICENSES_EXTRA=<dir of license folders> (share/licenses)
#   D3DMETAL=<GPTK 4.0 beta 3: dir with external/ and wine/> D3DMETAL_GPTK3=<GPTK 3.0: the same, from redist/lib> MEDIA=<x86_64 GStreamer/FFmpeg prefix> SIGN_IDENTITY="Developer ID Application: ..." \
#     tools/package/build-wine-x86_64-app.sh <output dir>
set -euo pipefail

OUT_DIR=${1:?usage: build-wine-x86_64-app.sh <output dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
: "${BUNDLES:?} ${MULLION:?} ${DXVK:?} ${DXVK_I386:?} ${VKD3D:?} ${MOLTENVK:?} ${NGX:?} ${D3DMETAL:?} ${D3DMETAL_GPTK3:?} ${MEDIA:?}"
IDENTITY=${SIGN_IDENTITY:--}
VERSION=${VERSION:-5.2}
BUNDLE_ID=${BUNDLE_ID:-com.metalsharp.wine.x86_64}
ICON=MetalSharp-Wine
WORK="$OUT_DIR/.work"
APP="$OUT_DIR/MetalSharp-Wine-x86_64.app"
C="$APP/Contents"
M="$C/share/metalsharp"

need() { [ -e "$1" ] || { echo "missing: $1" >&2; exit 1; }; }
BUNDLE_NAMES=(metalsharp-runtime metalsharp-graphics-dll metalsharp-assets metalsharp-scripts-tools metalsharp-steam
              fnalibs goldberg)
for f in "${BUNDLE_NAMES[@]}"; do need "$BUNDLES/$f.tar.zst"; done
need "$MULLION/x86_64-unix/winemetal.so"
need "$DXVK/x86_64-windows/dxgi.dll"
need "$DXVK_I386/i386-windows/d3d9.dll"
need "$VKD3D/x86_64-windows/d3d12core.dll"
need "$MOLTENVK/libMoltenVK.dylib"
need "$NGX/nvngx.dll"
need "$D3DMETAL/external/D3DMetal.framework"
need "$D3DMETAL_GPTK3/external/D3DMetal.framework"
need "$D3DMETAL_GPTK3/wine/x86_64-windows/nvngx-on-metalfx.dll"
need "$MEDIA/lib/libgstreamer-1.0.0.dylib"
need "$MEDIA/lib/libavcodec.61.dylib"

# --- the bundles, unpacked ------------------------------------------------------------------------------------------
rm -rf "$WORK" "$APP"
mkdir -p "$WORK"
for f in "${BUNDLE_NAMES[@]}"; do
  mkdir -p "$WORK/$f"
  zstd -dc "$BUNDLES/$f.tar.zst" | tar -xf - -C "$WORK/$f"
done
RT="$WORK/metalsharp-runtime/runtime"
GX="$WORK/metalsharp-graphics-dll/Graphics/dll"
AS="$WORK/metalsharp-assets/assets"
xattr -crs "$WORK" 2>/dev/null || true

# --- the Wine root --------------------------------------------------------------------------------------------------
mkdir -p "$C/MacOS" "$M/wine-bin"
for f in "$RT/wine/bin/"*; do
  if file -b "$f" | grep -q Mach-O; then cp -p "$f" "$C/MacOS/"; else cp -p "$f" "$M/wine-bin/"; fi
done
ln -s MacOS "$C/bin"
# Wine finds its tools and libraries from its own path: bin/../lib, bin/../share
cp -Rp "$RT/wine/lib" "$RT/wine/share" "$RT/wine/include" "$C/"
rm -rf "$C/lib64"  # empty in the bundle

# The x86_64 Wine's modules rebuilt with MetalSharp Wine's fixes (merged-wine-11.17, branch metalsharp-fex-fixes):
# each module's unix library and its x86_64 and i386 PE builds, over the bundle's
if [ -n "${WINE_BUILD:-}" ]; then
  for m in ${WINE_MODULES:-}; do
    if [ "$m" = server ]; then cp -p "$WINE_BUILD/server/wineserver" "$C/MacOS/wineserver"; continue; fi
    if [ "$m" = loader ]; then cp -p "$WINE_BUILD/loader/wine" "$C/MacOS/wine"; continue; fi
    name=${m%.drv}; found=0
    for a in x86_64-windows i386-windows; do
      for f in "$WINE_BUILD/dlls/$m/$a/$m" "$WINE_BUILD/dlls/$m/$a/$m.dll"; do
        [ -f "$f" ] && { cp -p "$f" "$C/lib/wine/$a/"; found=1; break; }
      done
    done
    [ -f "$WINE_BUILD/dlls/$m/$name.so" ] && { cp -p "$WINE_BUILD/dlls/$m/$name.so" "$C/lib/wine/x86_64-unix/"; found=1; }
    [ $found = 1 ] || { echo "no build of $m in $WINE_BUILD" >&2; exit 1; }
  done
fi

# GStreamer (winegstreamer.so) and FFmpeg (winedmo.so): their libraries in lib, GStreamer's plugins in
# lib/gstreamer-1.0 and its plugin scanner in libexec/gstreamer-1.0, where GStreamer looks relative to libgstreamer
for f in "$MEDIA"/lib/*.dylib; do cp -P "$f" "$C/lib/"; done
mkdir -p "$C/lib/gstreamer-1.0" "$C/libexec/gstreamer-1.0"
cp -P "$MEDIA"/lib/gstreamer-1.0/*.dylib "$C/lib/gstreamer-1.0/"
cp -p "$MEDIA"/libexec/gstreamer-1.0/gst-plugin-scanner "$C/libexec/gstreamer-1.0/"
# winegstreamer.so names libintl.dylib bare
[ -e "$C/lib/libintl.dylib" ] || ln -s libintl.8.dylib "$C/lib/libintl.dylib"

# etc: the runtime's, with the conf files MetalSharp keeps per lane. dxmt.conf stays for older MetalSharp builds.
cp -Rp "$RT/wine/etc" "$M/etc"
cp -p "$AS/wine/etc/dxmt.conf" "$M/etc/dxmt.conf"
cp -p "$HERE/wine-x86_64/mullion.conf" "$HERE/wine-x86_64/vkd3d.conf" "$HERE/wine-x86_64/dxvk.conf" "$M/etc/"

# --- graphics lanes -------------------------------------------------------------------------------------------------
builtin() {  # marks PE files as Wine builtins, which Wine loads from its DLL path in place of System32's
  python3 -I - "$@" <<'PY'
import struct, sys
for path in sys.argv[1:]:
    with open(path, "r+b") as f:
        header = f.read(64)
        assert header[:2] == b"MZ" and struct.unpack_from("<I", header, 60)[0] >= 96, path
        f.seek(64)
        f.write(b"Wine builtin DLL".ljust(32, b"\0"))
PY
}

# Mullion as the DXMT lane, and as Wine's own winemetal (the PE and unix halves must match)
rm -rf "$C/lib/dxmt"
cp -Rp "$MULLION" "$C/lib/dxmt"
printf '{"schema":"metalsharp-dxmt-runtime-v1","version":"%s","source":"bundled:MetalSharp-Wine-x86_64.app"}' \
  "$(sed -n 's/^Mullion \([0-9a-f]*\).*/mullion-\1/p' "$MULLION/VERSION")" > "$C/lib/dxmt/metalsharp-dxmt-runtime.json"
cp -p "$MULLION/x86_64-windows/winemetal.dll" "$C/lib/wine/x86_64-windows/winemetal.dll"
# NGX loads a core only if System32's nvngx.dll is signed by a certificate the game's DXGI trusts (Mullion's and
# DXVK-MacOS's both trust Mullion NGX, VKD3D-Proton-MacOS's CI key), then Wine loads the lane's own core by name. The
# NGX core and NVAPI (NGX: VKD3D-Proton-MacOS's x86_64 release, Wine builtins, signed by its CI) are Wine builtins,
# which wineboot puts in every prefix's System32 (wine.inf); without them System32 has only a placeholder
cp -p "$NGX/nvngx.dll" "$NGX/nvapi64.dll" "$C/lib/wine/x86_64-windows/"
cp -p "$MULLION/i386-windows/winemetal.dll" "$C/lib/wine/i386-windows/winemetal.dll"
cp -p "$MULLION/x86_64-unix/winemetal.so" "$C/lib/wine/x86_64-unix/winemetal.so"

# The VKD3D lane, D3D12 to D3D8 on MoltenVK, from the VKD3D-Proton-MacOS and DXVK-MacOS releases as they are.
# Its D3D12 half: VKD3D-Proton's d3d12/d3d12core, DXVK-MacOS's DXGI (and d3d11/d3d10core, which the backend stages
# from here too), DLSS's NGX core and NVAPI
L="$M/vkd3d/vkd3d-proton"
mkdir -p "$L/x86_64-windows"
cp -p "$VKD3D/x86_64-windows/d3d12.dll" "$VKD3D/x86_64-windows/d3d12core.dll" "$L/x86_64-windows/"
cp -p "$DXVK/x86_64-windows/dxgi.dll" "$DXVK/x86_64-windows/d3d11.dll" "$DXVK/x86_64-windows/d3d10core.dll" \
  "$L/x86_64-windows/"
cp -p "$NGX/nvngx.dll" "$NGX/nvapi64.dll" "$L/x86_64-windows/"
builtin "$L/x86_64-windows/nvngx.dll" "$L/x86_64-windows/nvapi64.dll"
for f in "$VKD3D"/LICENSE* "$VKD3D"/SHA256SUMS "$VKD3D"/licenses; do [ -e "$f" ] && cp -Rp "$f" "$L/"; done

# Its D3D11 to D3D8 half: DXVK-MacOS (x86_64 and i386) with the same NGX core and NVAPI, and the bundle's D3D9
# MoltenVK settings
L="$M/vkd3d/dxvk"
mkdir -p "$L/x86_64-windows" "$L/i386-windows"
cp -p "$DXVK"/x86_64-windows/*.dll "$L/x86_64-windows/"
cp -p "$DXVK_I386"/i386-windows/*.dll "$L/i386-windows/"
cp -p "$NGX/nvngx.dll" "$NGX/nvapi64.dll" "$L/x86_64-windows/"
builtin "$L/x86_64-windows/nvngx.dll" "$L/x86_64-windows/nvapi64.dll"
cp -p "$GX/dxvk/dxvk.conf" "$L/dxvk.conf"
for f in "$DXVK"/LICENSE "$DXVK"/LICENSE.dxbc-spirv "$DXVK"/SHA256SUMS; do [ -e "$f" ] && cp -p "$f" "$L/"; done
# Wine loads the lane by itself for GRAPHICS_BACKEND=vkd3d (ntdll's VKD3D lane resolver, as on MetalSharp Wine), from
# its builtin path, which takes only DLLs with Wine's builtin marker
builtin "$M"/vkd3d/vkd3d-proton/x86_64-windows/*.dll "$L"/x86_64-windows/*.dll "$L"/i386-windows/*.dll
basename "$DXVK" | sed 's/^dxvk-macos-\(.*\)-x86_64$/DXVK-MacOS \1/' > "$L/VERSION"

# MoltenVK: one universal build, its Metal Shader Converter and DXBC converter next to it; the VKMT lane's
# directory points at the same files
U="$C/lib/wine/x86_64-unix"
rm -f "$U/libMoltenVK.dylib" "$U/libMoltenVK.1.dylib"
cp -p "$MOLTENVK/libMoltenVK.dylib" "$U/libMoltenVK.1.dylib"
ln -s libMoltenVK.1.dylib "$U/libMoltenVK.dylib"
# Wine 11.17's winevulkan opens libvulkan.dylib (SONAME_LIBVULKAN), which the bundle has as an older x86_64-only
# MoltenVK without geometry shaders: every Vulkan lane takes this MoltenVK through it
rm -f "$U/libvulkan.dylib"
ln -s libMoltenVK.1.dylib "$U/libvulkan.dylib"
for f in libmetalirconverter.dylib libdxbcconverter.dylib; do
  [ -f "$MOLTENVK/$f" ] && ditto "$MOLTENVK/$f" "$U/$f"
done
V="$C/lib/moltenvk-vkmt"
rm -f "$V/libMoltenVK.dylib" "$V/libMoltenVK.1.dylib"
ln -s ../wine/x86_64-unix/libMoltenVK.1.dylib "$V/libMoltenVK.dylib"
ln -s ../wine/x86_64-unix/libMoltenVK.1.dylib "$V/libMoltenVK.1.dylib"
for f in libmetalirconverter.dylib libdxbcconverter.dylib; do
  [ -f "$U/$f" ] && ln -s "../wine/x86_64-unix/$f" "$V/$f"
done

# SDL2 for winebus (controllers), as setup links it: the universal SDL2 from the shims
ln -sf ../share/metalsharp/runtime/shims/libSDL2-2.0.0.dylib "$C/lib/libSDL2-2.0.0.dylib"

# --- ~/.metalsharp/runtime/<name> -----------------------------------------------------------------------------------
R="$M/runtime"
mkdir -p "$R"
cp -Rp "$RT/host" "$RT/launchers" "$R/"
cp -p "$RT/metalsharp-backend" "$R/"
ditto "$D3DMETAL" "$R/d3dmetal-gptk4-beta3"
ditto "$D3DMETAL_GPTK3" "$R/d3dmetal-gptk3"
cp -Rp "$GX/wfdxcompat" "$GX/wfdxcompat-agility" "$R/"
for d in mono-x86 mono-arm64 dxvk-1.10.3 goldberg shims fnalibs fna-kickstart faudio; do cp -Rp "$AS/$d" "$R/$d"; done
# fnalibs' SDL2-linked FNA3D, FAudio, theorafile and FMOD (the fnalibs bundle) over the asset bundle's
cp -Rp "$WORK/fnalibs/fnalibs/." "$R/fnalibs/"
mkdir -p "$R/steam-bridge"
cp -p "$R/shims/libsteam_api.dylib" "$R/steam-bridge/libsteam_api.dylib"
cp -Rp "$AS/shader-cache" "$M/shader-cache"
mkdir -p "$M/scripts" "$M/assets"
cp -Rp "$WORK/metalsharp-scripts-tools/scripts/tools" "$M/scripts/tools"
cp -Rp "$WORK/metalsharp-steam/steam" "$M/steam"
for d in gptk sdl3 xna unity-mono prebuilt-launchers; do [ -d "$AS/$d" ] && cp -Rp "$AS/$d" "$M/assets/$d"; done
# the asset bundle's GPTK copy links its unix modules to ../../external, one level too far up
for f in "$M/assets/gptk/x86_64-unix/"*; do
  [ -L "$f" ] && [ ! -e "$f" ] && ln -sfn ../external/libd3dshared.dylib "$f"
done
# the goldberg bundle's steamclient next to the asset bundle's steam_api DLLs (newer than the goldberg bundle's,
# and the ones setup installs); the goldberg bundle's own steam_api DLLs stay in assets/goldberg-bundle
cp -Rp "$WORK/goldberg/steamclient" "$R/goldberg/steamclient"
cp -Rp "$WORK/goldberg" "$M/assets/goldberg-bundle"
mkdir -p "$M/graphics"
cp -Rp "$GX/d3dmetal" "$GX/external" "$M/graphics/"
[ -f "$BUNDLES/metalsharp-bundle-manifest.tsv" ] && cp -p "$BUNDLES/metalsharp-bundle-manifest.tsv" "$M/bundle-manifest.tsv"

# --- relocatable: libraries named by their build machines' paths are found next to their users ----------------------
python3 -I "$HERE/wine-x86_64/relocate-macho.py" "$C"

# The app's main executable is Contents/MacOS/wine, whose signature is bound to the app's Info.plist; outside the app
# (the x86_64 install's runtime/wine/bin, laid out from it) that copy does not run. A copy signed on its own goes in
# wine-bin, which setup lays over runtime/wine/bin.
cp -p "$C/MacOS/wine" "$M/wine-bin/wine"

# --- the app --------------------------------------------------------------------------------------------------------
mkdir -p "$C/Resources"
# actool (the icon) comes with Xcode, not the command line tools
xcrun -f actool >/dev/null 2>&1 || export DEVELOPER_DIR=${DEVELOPER_DIR:-$HOME/Downloads/Xcode-beta.app/Contents/Developer}
xcrun actool "$HERE/wine-x86_64/$ICON.icon" --compile "$C/Resources" --app-icon "$ICON" \
  --platform macosx --minimum-deployment-target 15.0 --output-partial-info-plist "$WORK/icon-partial.plist" >/dev/null
# WineMetalGL (OpenGL 1.0-4.6 on Metal; WINEMETALGL: MetalSharp Wine's build-winemetalgl-x86_64.sh output), the
# x86_64 Wine's default OpenGL as on MetalSharp Wine: its module in place of the bundle's and opengl32 forwarders in
# lib/winemetalgl, which ntdll serves for opengl32.dll
if [ -n "${WINEMETALGL:-}" ]; then
  cp -p "$WINEMETALGL"/wine/x86_64-windows/winemetalgl.dll "$C/lib/wine/x86_64-windows/"
  cp -p "$WINEMETALGL"/wine/i386-windows/winemetalgl.dll "$C/lib/wine/i386-windows/"
  cp -p "$WINEMETALGL"/wine/x86_64-unix/winemetalgl.so "$WINEMETALGL"/wine/x86_64-unix/libwinemetalgl.dylib "$C/lib/wine/x86_64-unix/"
  rm -rf "$C/lib/winemetalgl"; cp -Rp "$WINEMETALGL/lib" "$C/lib/winemetalgl"
fi

# Microsoft's x64 VC++ 2015-2022 runtime DLLs (VCRUN_X64, from vc_redist.x64.exe): prefixes shared with MetalSharp
# Wine hold the ARM64X ones its installer puts in under FEX, which this Wine cannot run; the backend swaps these in
# (ms_wine_fit_prefix_vcrun)
if [ -n "${VCRUN_X64:-}" ]; then
  mkdir -p "$M/redist/vcrun2022-x64" "$C/share/licenses/vcrun2022"
  cp -p "$VCRUN_X64"/*.dll "$VCRUN_X64"/VERSION "$M/redist/vcrun2022-x64/"
  [ -f "$VCRUN_X64/LICENSE.rtf" ] && cp -p "$VCRUN_X64/LICENSE.rtf" "$C/share/licenses/vcrun2022/"
fi

# Programs run from the x86_64 install's runtime/wine/bin/wine, outside any app bundle: winemac.drv shows them with
# share/wine/MetalSharp-Wine.icns in the Dock (MetalSharp Wine's fix), as the app's own programs show its icon
cp -p "$C/Resources/$ICON.icns" "$C/share/wine/$ICON.icns"

# --- licenses: everything the app redistributes, in share/licenses (runtime/wine/share/licenses when installed) ----
#   DEPS_SRC: MetalSharp Wine's deps/src-x86_64 (what build-deps-x86_64.sh built: FFmpeg, GStreamer, GLib, ...) and
#   deps/src (the same projects' sources for the bundle's FreeType, GnuTLS, libpng, SDL2, MoltenVK, Vulkan loader...);
#   LICENSES_EXTRA: license folders with no source tree here (brotli, bzip2, DXC, Metal Shader Converter)
LIC="$C/share/licenses"
mkdir -p "$LIC"
license_from() {  # license_from <name> <source dir>: the source's license files into share/licenses/<name>
  local f
  mkdir -p "$LIC/$1"
  for f in "$2"/*; do
    case "$(basename "$f" | tr '[:upper:]' '[:lower:]')" in
      copying*|license*|licence*|lgpl*|gpl*|copyright*|notice*) [ -f "$f" ] && cp -p "$f" "$LIC/$1/" ;;
    esac
  done
  [ "$1" = freetype ] && cp -p "$2"/docs/FTL.TXT "$2"/docs/GPLv2.TXT "$LIC/$1/" 2>/dev/null
  return 0
}
LICENSED=(ffmpeg gettext glib gstreamer gstbase gstgood gstbad gstlibav libffi pcre2
          freetype gmp gnutls nettle libtasn1 libpng sdl2 moltenvk vkloader)
IFS=: read -r -a DEPS_DIRS <<< "${DEPS_SRC:-}"
for p in "${LICENSED[@]}"; do
  for d in "${DEPS_DIRS[@]}"; do
    s=$(ls -d "$d/$p"/*/ 2>/dev/null | head -1 || true)
    [ -n "$s" ] && { license_from "$p" "${s%/}"; break; }
  done
  [ -d "$LIC/$p" ] || echo "warning: no license source for $p" >&2
done
[ -n "${LICENSES_EXTRA:-}" ] && cp -R "$LICENSES_EXTRA"/. "$LIC/"
mkdir -p "$LIC/wine" "$LIC/zlib" "$LIC/mullion" "$LIC/dxvk-macos" "$LIC/vkd3d-proton"
[ -n "${WINE_SRC:-}" ] && cp -p "$WINE_SRC/COPYING.LIB" "$WINE_SRC/LICENSE" "$WINE_SRC/AUTHORS" "$LIC/wine/"
[ -n "${WINE_SRC:-}" ] && cp -p "$WINE_SRC/libs/zlib/LICENSE" "$LIC/zlib/"
cp -p "$MULLION/LICENSE" "$LIC/mullion/"
cp -p "$DXVK"/LICENSE* "$LIC/dxvk-macos/"
cp -p "$VKD3D"/LICENSE* "$LIC/vkd3d-proton/"
chmod -R u+w "$LIC"
cat > "$C/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleExecutable</key><string>wine</string>
  <key>CFBundleIconFile</key><string>$ICON</string>
  <key>CFBundleIconName</key><string>$ICON</string>
  <key>CFBundleName</key><string>MetalSharp Wine</string>
  <key>CFBundleDisplayName</key><string>MetalSharp Wine (x86_64)</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>CFBundleGetInfoString</key><string>MetalSharp Wine $VERSION (Wine 11.17, x86_64, Rosetta)</string>
  <key>LSArchitecturePriority</key><array><string>x86_64</string></array>
  <key>LSMinimumSystemVersion</key><string>15.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
</dict></plist>
EOF

# Wine updates a prefix when wine.inf's modification time differs from the prefix's .update-timestamp: the bundles'
# files carry time 0, which would leave prefixes on a newer app's builtins never updated, so it is this build's time
touch "$C/share/wine/wine.inf"

# What it was made from
{
  echo "MetalSharp Wine $VERSION (x86_64)"
  echo "bundles: $(cd "$BUNDLES" && shasum -a 256 metalsharp-runtime.tar.zst metalsharp-graphics-dll.tar.zst \
    metalsharp-assets.tar.zst metalsharp-scripts-tools.tar.zst metalsharp-steam.tar.zst fnalibs.tar.zst \
    goldberg.tar.zst | tr '\n' ';')"
  echo "mullion: $(cat "$MULLION/VERSION")"
  echo "dxvk-macos: $(basename "$DXVK") $(shasum -a 256 "$DXVK/x86_64-windows/dxgi.dll" | cut -c1-12), $(basename "$DXVK_I386")"
  echo "vkd3d-proton: $(cat "$VKD3D/VERSION" 2>/dev/null || basename "$VKD3D")"
  echo "moltenvk: $(basename "$MOLTENVK")"
  [ -n "${WINE_BUILD:-}" ] && echo "wine: $(git -C "${WINE_SRC:-$WINE_BUILD/../../merged-wine-11.17}" describe --always --dirty 2>/dev/null || echo rebuilt) ($WINE_MODULES)"
  echo "media: $(ls "$MEDIA"/lib/libgstreamer-1.0.*.dylib | head -1 | xargs basename), $(ls "$MEDIA"/lib/libavcodec.*.*.dylib | head -1 | xargs basename)"
  echo "d3dmetal: $(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' \
    "$D3DMETAL/external/D3DMetal.framework/Resources/Info.plist") (macOS 27 and later), $(/usr/libexec/PlistBuddy \
    -c 'Print CFBundleShortVersionString' "$D3DMETAL_GPTK3/external/D3DMetal.framework/Resources/Info.plist") (below)"
} > "$M/SOURCES.txt"

# --- signing --------------------------------------------------------------------------------------------------------
# Wine needs JIT-style memory and loads libraries the app did not sign (games' own); MetalSharp passes library paths
# and inserted libraries to it in the environment, which the hardened runtime otherwise strips.
ENT="$WORK/wine-x86_64.entitlements"
cat > "$ENT" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>com.apple.security.cs.allow-jit</key><true/>
  <key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>
  <key>com.apple.security.cs.disable-executable-page-protection</key><true/>
  <key>com.apple.security.cs.disable-library-validation</key><true/>
  <key>com.apple.security.cs.allow-dyld-environment-variables</key><true/>
</dict></plist>
EOF
# No secure timestamp unless TIMESTAMP=1 (notarization needs one; a Developer ID signature does not): a timestamp is a
# round trip to Apple's server for each of the app's ~1500 Mach-O files
opts=(-f -s "$IDENTITY")
[ "$IDENTITY" != - ] && opts+=(-o runtime)
[ "$IDENTITY" != - ] && [ "${TIMESTAMP:-0}" = 1 ] && opts+=(--timestamp)
apple_signed() {  # (no grep -q in a pipe: under pipefail its early exit fails the pipeline)
  local info; info=$(codesign -dvv "$1" 2>&1 || true)
  [[ $'\n'"$info"$'\n' == *$'\nAuthority=Software Signing\n'* ]]
}
# Inside out: frameworks, then every other Mach-O (in parallel: none of them contains another), then the main
# executable and the bundle. Apple-signed code (D3DMetal, libd3dshared, Metal Shader Converter) keeps Apple's signature.
find "$C" -name '*.framework' -type d -prune | while read -r fw; do
  apple_signed "$fw" || codesign "${opts[@]}" "$fw"
done
# the Mach-O files, by their magic in one pass (file(1) on each of the tree's files took minutes)
python3 -I - "$C" > "$WORK/macho.list" <<'PY'
import os, sys
magics = {b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xfe\xed\xfa\xce", b"\xca\xfe\xba\xbe"}
root = sys.argv[1]
for d, dirs, files in os.walk(root):
    dirs[:] = [x for x in dirs if not x.endswith(".framework")]
    for n in files:
        p = os.path.join(d, n)
        if os.path.islink(p) or p == os.path.join(root, "MacOS", "wine"):
            continue
        try:
            with open(p, "rb") as f:
                if f.read(4) in magics:
                    sys.stdout.write(p + "\0")
        except OSError:
            pass
PY
# the worker: signs its files unless Apple signed them; executables get the entitlements
cat > "$WORK/sign.sh" <<'SH'
#!/bin/bash
ent=$1; shift; opts=(-f -s "$SIGN_ID"); read -r -a flags <<< "$SIGN_FLAGS"; opts+=("${flags[@]}")
for f in "$@"; do
  info=$(codesign -dvv "$f" 2>&1 || true)
  [[ $'\n'"$info"$'\n' == *$'\nAuthority=Software Signing\n'* ]] && continue
  case "$(file -b "$f")" in
    *executable*) codesign "${opts[@]}" --entitlements "$ent" "$f" ;;
    *) codesign "${opts[@]}" "$f" ;;
  esac || { echo "codesign failed: $f" >&2; exit 255; }
done 2>&1 | grep -v 'replacing existing signature' >&2
exit "${PIPESTATUS[0]}"
SH
echo "signing $(tr -cd '\0' < "$WORK/macho.list" | wc -c | tr -d ' ') Mach-O files"
SIGN_ID="$IDENTITY" SIGN_FLAGS="${opts[*]:3}" xargs -0 -P "$(sysctl -n hw.ncpu)" -n 32 bash "$WORK/sign.sh" "$ENT" < "$WORK/macho.list"
codesign "${opts[@]}" --entitlements "$ENT" "$C/MacOS/wine"
codesign "${opts[@]}" --entitlements "$ENT" "$APP"
codesign --verify --deep --strict "$APP"
rm -rf "$WORK"
echo "built: $APP ($(du -sh "$APP" | cut -f1), signed by $IDENTITY)"
