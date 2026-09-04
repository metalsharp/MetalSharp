#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SDK_DIR="$ROOT_DIR/tools/d3d12-metal-sdk"
BOUNDED_RUNNER="$SDK_DIR/scripts/run-bounded-command.py"
BUILD_DIR="${METALSHARP_DXMT_BUILD_DIR:-$ROOT_DIR/vendor/dxmt/build-metalsharp-x64}"
WINE_ROOT="${METALSHARP_WINE_ROOT:-$HOME/.metalsharp/runtime/wine}"
WINE_BIN="$WINE_ROOT/bin/wine"
WINE_SERVER="$WINE_ROOT/bin/wineserver"
PROFILE="${METALSHARP_PROBE_PROFILE:-phase6-exhaustive}"
RESULTS_DIR="${METALSHARP_PHASE6_RESULTS_DIR:-$SDK_DIR/results}"
JOBS="${METALSHARP_BUILD_JOBS:-8}"
BUILD_RUNTIME=0
WITH_RASTERIZATION=0
WITH_ROV_DIMENSIONS=0
WITH_ROV_MSAA=0
WITH_SAMPLE_POSITIONS=0
WITH_VIEW_INSTANCING=0
WITH_FIXED_FUNCTION=0
WITH_MSAA=0
WITH_HOST_INVENTORY=0
MSAA_TIMEOUT_SECONDS=120
KEEP_SANDBOX_ON_FAILURE=0

usage() {
  cat <<'EOF'
Usage: run-phase6-exhaustive.sh [options]

Builds source-owned Phase 6 probes, stages a matching runtime below a
throw-away directory, validates the Winemetal bridge before execution, and
runs the exact interpolation provider probe.  No persistent Wine runtime or
prefix is modified.

Options:
  --build-runtime       Build the configured DXMT tree and repair the final
                        Winemetal artifact after Meson's symbol extractor.
  --with-rasterization Also run the point/line breadth probe.
  --with-rov-dimensions Also run the 1D/1D-array/3D ROV probe.
  --with-rov-msaa      Also run the flattened MSAA ROV ordering probe.
  --with-sample-positions Also run programmable 4x4 positions and reset.
  --with-view-instancing Also run the four-view/mask-zero replay probe.
  --with-fixed-function Also run the established fixed-function graphics matrix.
  --with-msaa          Also run the writable-MSAA/sample-position matrix.
  --with-host-inventory Compile/run the native Metal interpolation inventory.
  --profile NAME        Result profile (default phase6-exhaustive).
  --keep-sandbox-on-failure Preserve the disposable sandbox for diagnosis;
                        normal runs always remove it.
  --help                Show this help.

The runtime build is not enabled by default: use --build-runtime when a fresh
source build is required.  Both modes always build the probe executables in
the disposable sandbox.
EOF
}

while (($#)); do
  case "$1" in
    --build-runtime) BUILD_RUNTIME=1 ;;
    --with-rasterization) WITH_RASTERIZATION=1 ;;
    --with-rov-dimensions) WITH_ROV_DIMENSIONS=1 ;;
    --with-rov-msaa) WITH_ROV_MSAA=1 ;;
    --with-sample-positions) WITH_SAMPLE_POSITIONS=1 ;;
    --with-view-instancing) WITH_VIEW_INSTANCING=1 ;;
    --with-fixed-function) WITH_FIXED_FUNCTION=1 ;;
    --with-msaa) WITH_MSAA=1 ;;
    --with-host-inventory) WITH_HOST_INVENTORY=1 ;;
    --keep-sandbox-on-failure) KEEP_SANDBOX_ON_FAILURE=1 ;;
    --profile)
      (($# >= 2)) || { echo "--profile requires a value" >&2; exit 2; }
      PROFILE="$2"; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

[[ -x "$WINE_BIN" ]] || { echo "missing Wine binary: $WINE_BIN" >&2; exit 2; }
[[ -x "$WINE_SERVER" ]] || { echo "missing wineserver: $WINE_SERVER" >&2; exit 2; }
[[ -d "$BUILD_DIR" ]] || { echo "missing DXMT build directory: $BUILD_DIR" >&2; exit 2; }
[[ "$($WINE_BIN --version)" == "wine-11.5" ]] || {
  echo "expected Wine 11.5 from $WINE_BIN" >&2
  exit 2
}

# Refuse to overwrite a developer's pre-existing generated artifacts.  The
# --build-runtime cleanup below is therefore safe and deterministic.
BUILD_ARTIFACTS=(
  "$BUILD_DIR/src/d3d10/d3d10core.dll"
  "$BUILD_DIR/src/d3d11/d3d11.dll"
  "$BUILD_DIR/src/d3d12/d3d12.dll"
  "$BUILD_DIR/src/dxgi/dxgi.dll"
  "$BUILD_DIR/src/dxgi/dxgi_dxmt.dll"
  "$BUILD_DIR/src/winemetal/winemetal.dll"
  "$BUILD_DIR/src/winemetal/unix/winemetal.so"
  "$BUILD_DIR/src/nvapi/nvapi64.dll"
  "$BUILD_DIR/src/nvngx/nvngx.dll"
)
for artifact in "${BUILD_ARTIFACTS[@]}"; do
  if git -C "$ROOT_DIR" status --short -- "$artifact" | grep -q .; then
    echo "generated build artifact is already dirty; refusing to overwrite: $artifact" >&2
    exit 2
  fi
done

SANDBOX="$(mktemp -d /private/tmp/metalsharp-phase6-exhaustive.XXXXXX)"
PREFIX="$SANDBOX/prefix"
WORK="$SANDBOX/work"
LOG_DIR="$SANDBOX/logs"
CACHE_DIR="$SANDBOX/shader-cache"
mkdir -p "$PREFIX" "$WORK" "$LOG_DIR" "$CACHE_DIR" "$RESULTS_DIR"
BUILD_MUTATED=0
D3D12_ALIAS=""
UNIX_ALIAS=""

cleanup() {
  status=$?
  if [[ -n "$UNIX_ALIAS" ]]; then
    rm -f "$SANDBOX/wine/lib/wine/x86_64-unix/$UNIX_ALIAS"
  fi
  WINEPREFIX="$PREFIX" "$WINE_SERVER" -k >/dev/null 2>&1 || true
  WINEPREFIX="$PREFIX" "$WINE_SERVER" -w >/dev/null 2>&1 || true
  if [[ "$BUILD_MUTATED" == "1" ]]; then
    # These paths were proven clean before --build-runtime began.
    git -C "$ROOT_DIR" checkout -- \
      vendor/dxmt/build-metalsharp-x64/src/d3d10/d3d10core.dll \
      vendor/dxmt/build-metalsharp-x64/src/d3d11/d3d11.dll \
      vendor/dxmt/build-metalsharp-x64/src/d3d12/d3d12.dll \
      vendor/dxmt/build-metalsharp-x64/src/dxgi/dxgi.dll \
      vendor/dxmt/build-metalsharp-x64/src/dxgi/dxgi_dxmt.dll \
      vendor/dxmt/build-metalsharp-x64/src/winemetal/winemetal.dll \
      vendor/dxmt/build-metalsharp-x64/src/winemetal/unix/winemetal.so \
      vendor/dxmt/build-metalsharp-x64/src/nvapi/nvapi64.dll \
      vendor/dxmt/build-metalsharp-x64/src/nvngx/nvngx.dll \
      >/dev/null 2>&1 || status=1
  fi
  if [[ "$KEEP_SANDBOX_ON_FAILURE" == "1" && "$status" != "0" ]]; then
    echo "Phase 6 sandbox preserved for diagnosis: $SANDBOX" >&2
  else
    rm -rf "$SANDBOX"
  fi
  if [[ "$KEEP_SANDBOX_ON_FAILURE" != "1" && -e "$SANDBOX" ]]; then
    echo "failed to remove Phase 6 sandbox: $SANDBOX" >&2
    status=1
  fi
  exit "$status"
}
trap cleanup EXIT INT TERM HUP

if [[ "$BUILD_RUNTIME" == "1" ]]; then
  # Mark the generated paths before invoking the build as well as after it:
  # a failed compiler/link step must still restore the clean precondition.
  BUILD_MUTATED=1
  # Meson's symbol-extractor target can rewrite Winemetal to a reduced export
  # set.  Build the complete tree first, then relink only the final builtin
  # target so staging sees the actual export-complete PE artifact.
  meson compile -C "$BUILD_DIR" -j "$JOBS"
  # Clean the Unix half as well.  Meson does not always register
  # winemetal.h as a dependency of the C/ObjC Unix object; leaving that object
  # in place can pair a current PE command enum with an older Unix switch
  # table, producing render_cmd_unknown and a poisoned command buffer.
  ninja -C "$BUILD_DIR" -t clean src/winemetal/winemetal.dll \
    src/winemetal/unix/winemetal.so >/dev/null
  ninja -C "$BUILD_DIR" src/winemetal/winemetal.dll \
    src/winemetal/unix/winemetal.so >/dev/null

  # Relink consumers after the complete Winemetal import library exists.  The
  # normal dependency walk invokes Meson's symbolextractor first; that target
  # can replace the build-tree Winemetal PE with its reduced export view and
  # leaves consumers bound to a stale ordinal/import-library snapshot.  Use
  # Ninja's already-generated link command directly, without running another
  # extractor or postprocessor.
  relink_only() {
    local target="$1"
    local link_command
    link_command="$(ninja -C "$BUILD_DIR" -t commands "$target" | tail -1)"
    [[ -n "$link_command" ]] || {
      echo "could not recover link command for $target" >&2
      return 1
    }
    (cd "$BUILD_DIR" && bash -c "$link_command")
  }
  relink_only src/dxgi/dxgi_dxmt.dll
  relink_only src/d3d12/d3d12.dll
fi

python3 "$SDK_DIR/scripts/stage-phase6-sandbox.py" \
  --build-dir "$BUILD_DIR" \
  --sandbox-root "$SANDBOX" \
  --wine-root "$WINE_ROOT" \
  --profile "$PROFILE" \
  --results-dir "$RESULTS_DIR"

STAGE_MANIFEST="$RESULTS_DIR/stage-phase6-sandbox-$PROFILE.json"
RUNTIME_DIR="$SANDBOX/runtime"
SANDBOX_WINE="$SANDBOX/wine"
ABI_RESULT="$RESULTS_DIR/winemetal-abi-$PROFILE.json"
# ABI validation is deliberately before any DXIL compilation or draw.  It
# checks the selected runtime, sandbox builtin, and disposable prefix copies
# as well as source call-table/struct-size provenance.
python3 "$SDK_DIR/scripts/check-winemetal-abi.py" \
  --profile "$PROFILE" --dxmt-runtime "$RUNTIME_DIR" \
  --wine-runtime "$SANDBOX_WINE" --prefix "$PREFIX" \
  --results-dir "$RESULTS_DIR" --optional-prefix

# Build exactly the probes used by this gate in the disposable work tree.
CXX="${CXX:-x86_64-w64-mingw32-g++}"
probe_flags=(
  -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -Wall -Wextra -Werror
  -include "$SDK_DIR/probes/probe_runtime.hpp"
)
"$CXX" "${probe_flags[@]}" \
  "$SDK_DIR/probes/probe_interpolation/probe_interpolation.cpp" \
  -o "$WORK/probe_interpolation.exe"
"$CXX" "${probe_flags[@]}" \
  "$SDK_DIR/probes/probe_phase6_invalid_descriptors.cpp" \
  -o "$WORK/probe_phase6_invalid_descriptors.exe"
if [[ "$WITH_RASTERIZATION" == "1" ]]; then
  "$CXX" "${probe_flags[@]}" \
    "$SDK_DIR/probes/probe_rasterization_breadth/probe_rasterization_breadth.cpp" \
    -o "$WORK/probe_rasterization_breadth.exe"
fi
if [[ "$WITH_ROV_DIMENSIONS" == "1" ]]; then
  "$CXX" "${probe_flags[@]}" \
    "$SDK_DIR/probes/probe_rov_dimensions/probe_rov_dimensions.cpp" \
    -o "$WORK/probe_rov_dimensions.exe"
fi
if [[ "$WITH_ROV_MSAA" == "1" ]]; then
  "$CXX" "${probe_flags[@]}" \
    "$SDK_DIR/probes/probe_rov_msaa.cpp" \
    -o "$WORK/probe_rov_msaa.exe"
fi
if [[ "$WITH_SAMPLE_POSITIONS" == "1" ]]; then
  "$CXX" "${probe_flags[@]}" \
    "$SDK_DIR/probes/probe_sample_positions_breadth.cpp" \
    -o "$WORK/probe_sample_positions_breadth.exe"
fi
if [[ "$WITH_VIEW_INSTANCING" == "1" ]]; then
  "$CXX" "${probe_flags[@]}" \
    "$SDK_DIR/probes/probe_view_instancing_breadth.cpp" \
    -o "$WORK/probe_view_instancing_breadth.exe"
  "$CXX" "${probe_flags[@]}" \
    -DMINI_PROBE_CASE=20 "-DMINI_PROBE_NAME=\"view_id_instancing\"" \
    "$SDK_DIR/probes/probe_mini_suite/probe_mini_suite.cpp" \
    -o "$WORK/probe_mini_view_id_instancing.exe"
fi
if [[ "$WITH_FIXED_FUNCTION" == "1" ]]; then
  "$CXX" "${probe_flags[@]}" \
    "$SDK_DIR/probes/probe_graphics_pso/probe_graphics_pso.cpp" \
    -o "$WORK/probe_graphics_pso.exe"
  "$CXX" "${probe_flags[@]}" \
    -DMINI_PROBE_CASE=18 "-DMINI_PROBE_NAME=\"inner_coverage\"" \
    "$SDK_DIR/probes/probe_mini_suite/probe_mini_suite.cpp" \
    -o "$WORK/probe_mini_inner_coverage.exe"
  "$CXX" "${probe_flags[@]}" \
    "$SDK_DIR/probes/probe_conservative_msaa.cpp" \
    -o "$WORK/probe_conservative_msaa.exe"
  "$CXX" "${probe_flags[@]}" \
    "$SDK_DIR/probes/probe_independent_logic_breadth.cpp" \
    -o "$WORK/probe_independent_logic_breadth.exe"
fi
if [[ "$WITH_MSAA" == "1" ]]; then
  "$CXX" "${probe_flags[@]}" \
    "$SDK_DIR/probes/probe_writable_msaa/probe_writable_msaa.cpp" \
    -o "$WORK/probe_writable_msaa.exe"
  "$CXX" "${probe_flags[@]}" \
    "$SDK_DIR/probes/probe_graphics_msaa_breadth.cpp" \
    -o "$WORK/probe_graphics_msaa_breadth.exe"
  "$CXX" "${probe_flags[@]}" \
    "$SDK_DIR/probes/probe_graphics_msaa_depth.cpp" \
    -o "$WORK/probe_graphics_msaa_depth.exe"
fi
HOST_STATUS=0
CAPS_STATUS=0
if [[ "$WITH_HOST_INVENTORY" == "1" ]]; then
  HOST_BIN="$WORK/probe-metal-interpolation"
  DEVELOPER_DIR="${DEVELOPER_DIR:-${METALSHARP_XCODE_ROOT:-/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer}}" \
    /usr/bin/xcrun --sdk macosx clang++ -std=c++17 -fobjc-arc \
    -framework Foundation -framework Metal \
    "$SDK_DIR/scripts/probe-metal-interpolation.mm" -o "$HOST_BIN"
  python3 "$BOUNDED_RUNNER" --timeout 30 --cwd "$WORK" \
    --output "$SANDBOX/host_inventory.json" \
    --stderr "$LOG_DIR/probe-host-inventory.stderr" -- \
    env DEVELOPER_DIR="${DEVELOPER_DIR:-${METALSHARP_XCODE_ROOT:-/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer}}" \
    "$HOST_BIN"
  HOST_STATUS=$?
fi

# Copy the pinned Agility/DXC files needed by the source-owned HLSL compile.
for file in dxc.exe dxcompiler.dll dxil.dll; do
  [[ -f "$SDK_DIR/out/bin/$file" ]] || { echo "missing SDK compiler file: $file" >&2; exit 2; }
  cp "$SDK_DIR/out/bin/$file" "$WORK/$file"
done
if [[ -d "$SDK_DIR/out/bin/D3D12" ]]; then
  cp -R "$SDK_DIR/out/bin/D3D12" "$WORK/D3D12"
fi
cp "$SDK_DIR/probes/probe_interpolation/interpolation.hlsl" "$WORK/interpolation.hlsl"
cp "$SDK_DIR/probes/probe_interpolation/interpolation_eval.hlsl" "$WORK/interpolation_eval.hlsl"
cp "$SDK_DIR/probes/probe_interpolation/interpolation_invalid.hlsl" "$WORK/interpolation_invalid.hlsl"
if [[ "$WITH_MSAA" == "1" ]]; then
  cp "$SDK_DIR/probes/graphics_msaa.hlsl" "$WORK/graphics_msaa.hlsl"
  cp "$SDK_DIR/probes/graphics_msaa_depth.hlsl" \
    "$WORK/graphics_msaa_depth.hlsl"
fi
if [[ "$WITH_ROV_DIMENSIONS" == "1" ]]; then
  cp "$SDK_DIR/probes/probe_rov_dimensions/rov_dimensions.hlsl" "$WORK/rov_dimensions.hlsl"
fi
if [[ "$WITH_ROV_MSAA" == "1" ]]; then
  cp "$SDK_DIR/probes/probe_rov_msaa.hlsl" "$WORK/probe_rov_msaa.hlsl"
fi
if [[ "$WITH_VIEW_INSTANCING" == "1" ]]; then
  cp "$SDK_DIR/probes/probe_view_id_instancing.hlsl" \
    "$WORK/probe_view_id_instancing.hlsl"
fi
if [[ "$WITH_FIXED_FUNCTION" == "1" ]]; then
  cp "$SDK_DIR/probes/probe_graphics_pso/conservative_raster.hlsl" \
    "$WORK/probe_conservative_raster.hlsl"
  cp "$SDK_DIR/probes/probe_graphics_pso/inner_coverage.hlsl" \
    "$WORK/probe_inner_coverage.hlsl"
fi

WINEDEBUG=-all WINEPREFIX="$PREFIX" "$WINE_BIN" wineboot -u >/dev/null 2>&1
mkdir -p "$PREFIX/drive_c/windows/system32"
for runtime_dll in "$RUNTIME_DIR/x86_64-windows"/*.dll; do
  [[ -f "$runtime_dll" ]] || continue
  cp "$runtime_dll" "$PREFIX/drive_c/windows/system32/$(basename "$runtime_dll")"
  cp "$runtime_dll" "$WORK/$(basename "$runtime_dll")"
done

# Use a unique app-local d3d12 alias and a unique Unix-library registration.
# Neither can collide with a stale Wine builtin, and both are removed by the
# trap above.
D3D12_ALIAS="d3d12-phase6-$RANDOM-$$.dll"
UNIX_ALIAS="winemetal-phase6-$RANDOM-$$.so"
cp "$RUNTIME_DIR/x86_64-windows/d3d12.dll" "$WORK/$D3D12_ALIAS"
ln -s "$RUNTIME_DIR/x86_64-unix/winemetal.so" \
  "$SANDBOX_WINE/lib/wine/x86_64-unix/$UNIX_ALIAS"

export WINEPREFIX="$PREFIX"
# Keep the selected DXMT route first while retaining the pinned Wine loader
# tree.  A bare disposable DXMT path hides ntdll.so from Wine itself and
# produces the misleading "could not exec the wine loader" error.
export WINEDLLPATH="$RUNTIME_DIR:$SANDBOX_WINE/lib/wine:$WINE_ROOT/lib/wine"
export WINEDLLOVERRIDES='d3d12=n,b;dxgi=n,b;d3d11=n,b;d3d10core=n,b;winemetal=n,b'
# Do not put the sandbox's ntdll.so/winemac.so in the host process's DYLD
# search path: that makes the Mach-O Wine launcher try to load a Windows
# Unix-loader half and report "could not exec the wine loader".  WINEDLLPATH
# handles Wine module lookup; DYLD only needs the pinned host Wine Unix tree.
export DYLD_LIBRARY_PATH="$WINE_ROOT/lib/wine/x86_64-unix"
export DXMT_WINEMETAL_UNIXLIB="$UNIX_ALIAS"
export DXMT_PROBE_D3D12_DLL="$D3D12_ALIAS"
# probe_writable_msaa launches DXC through CreateProcessA; provide an
# explicit Windows path so it cannot find a stale dxc.exe from the repository
# parent directory.
DXC_WIN_PATH="Z:${WORK//\//\\}/dxc.exe"
export D3D12_METAL_SDK_DXC="$DXC_WIN_PATH"
export DXMT_SHADER_CACHE_PATH="$CACHE_DIR"
export DXMT_LOG_PATH="$LOG_DIR"
export METAL_SHADER_CONVERTER=/nonexistent
export D3D12_METAL_SDK_PROFILE="$PROFILE"

# Compile source-owned DXIL in the same disposable prefix used by the probe.
for spec in \
  "vs_main vs_6_0 vs" \
  "ps_linear ps_6_0 linear" \
  "ps_noperspective ps_6_0 noperspective" \
  "ps_centroid ps_6_0 centroid" \
  "ps_sample ps_6_0 sample" \
  "ps_nointerpolation ps_6_0 flat"; do
  read -r entry target output <<<"$spec"
  # DXC is a standalone compiler.  Do not expose the disposable DXMT
  # WINEDLLPATH while launching it: Wine's loader itself must resolve ntdll
  # from its own installation, not from the application-only DXMT route.
  env -u WINEDLLPATH -u DYLD_LIBRARY_PATH -u DXMT_WINEMETAL_UNIXLIB \
    -u DXMT_PROBE_D3D12_DLL -u DXMT_SHADER_CACHE_PATH -u DXMT_LOG_PATH \
    WINEDEBUG=-all WINEPREFIX="$PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" "$WORK/dxc.exe" -nologo -E "$entry" -T "$target" \
    -Fo "$WORK/$output.cso" "$WORK/interpolation.hlsl" >/dev/null
 done
# The evaluation fixture is compiled separately so the PS contains DXIL
# evalCentroid/evalSampleIndex/evalSnapped calls rather than relying on a
# compiler-generated equivalent.
env -u WINEDLLPATH -u DYLD_LIBRARY_PATH -u DXMT_WINEMETAL_UNIXLIB \
  -u DXMT_PROBE_D3D12_DLL -u DXMT_SHADER_CACHE_PATH -u DXMT_LOG_PATH \
  WINEDEBUG=-all WINEPREFIX="$PREFIX" \
  WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
  "$WINE_BIN" "$WORK/dxc.exe" -nologo -E ps_eval -T ps_6_0 \
  -Fo "$WORK/evaluation.cso" "$WORK/interpolation_eval.hlsl" >/dev/null
env -u WINEDLLPATH -u DYLD_LIBRARY_PATH -u DXMT_WINEMETAL_UNIXLIB \
  -u DXMT_PROBE_D3D12_DLL -u DXMT_SHADER_CACHE_PATH -u DXMT_LOG_PATH \
  WINEDEBUG=-all WINEPREFIX="$PREFIX" \
  WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
  "$WINE_BIN" "$WORK/dxc.exe" -nologo -E ps_invalid -T ps_6_0 \
  -Fo "$WORK/invalid.cso" "$WORK/interpolation_invalid.hlsl" >/dev/null

if [[ "$WITH_FIXED_FUNCTION" == "1" ]]; then
  for spec in \
    "vs_main vs_6_0 probe_conservative_raster_vs" \
    "ps_main ps_6_0 probe_conservative_raster_ps" \
    "vs_main vs_6_0 probe_inner_coverage_vs" \
    "ps_main ps_6_0 probe_inner_coverage_ps"; do
    read -r entry target output <<<"$spec"
    input="$WORK/probe_conservative_raster.hlsl"
    [[ "$output" == probe_inner_coverage_* ]] && input="$WORK/probe_inner_coverage.hlsl"
    env -u WINEDLLPATH -u DYLD_LIBRARY_PATH -u DXMT_WINEMETAL_UNIXLIB \
      -u DXMT_PROBE_D3D12_DLL -u DXMT_SHADER_CACHE_PATH -u DXMT_LOG_PATH \
      WINEDEBUG=-all WINEPREFIX="$PREFIX" \
      WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" "$WORK/dxc.exe" -nologo -E "$entry" -T "$target" \
      -Fo "$WORK/$output.cso" "$input" >/dev/null
  done
fi

if [[ "$WITH_MSAA" == "1" ]]; then
  for spec in \
    "vs_main vs_6_0 graphics_msaa_vs" \
    "ps_main ps_6_7 graphics_msaa_ps" \
    "vs_main vs_6_0 graphics_msaa_depth_vs" \
    "ps_main ps_6_0 graphics_msaa_depth_ps"; do
    read -r entry target output <<<"$spec"
    input="$WORK/graphics_msaa.hlsl"
    [[ "$output" == graphics_msaa_depth_* ]] && input="$WORK/graphics_msaa_depth.hlsl"
    env -u WINEDLLPATH -u DYLD_LIBRARY_PATH -u DXMT_WINEMETAL_UNIXLIB \
      -u DXMT_PROBE_D3D12_DLL -u DXMT_SHADER_CACHE_PATH -u DXMT_LOG_PATH \
      WINEDEBUG=-all WINEPREFIX="$PREFIX" \
      WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" "$WORK/dxc.exe" -nologo -E "$entry" -T "$target" \
      -Fo "$WORK/$output.cso" "$input" >/dev/null
  done
fi

ROV_DIMENSIONS_COMPILE_STATUS=0
if [[ "$WITH_ROV_DIMENSIONS" == "1" ]]; then
  for spec in \
    "vs_main vs_6_0 rov_vs" \
    "ps_1d ps_6_0 rov_1d" \
    "ps_1d_array ps_6_0 rov_1d_array" \
    "ps_3d ps_6_0 rov_3d"; do
    read -r entry target output <<<"$spec"
    if ! env -u WINEDLLPATH -u DYLD_LIBRARY_PATH -u DXMT_WINEMETAL_UNIXLIB \
      -u DXMT_PROBE_D3D12_DLL -u DXMT_SHADER_CACHE_PATH -u DXMT_LOG_PATH \
      WINEDEBUG=-all WINEPREFIX="$PREFIX" \
      WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" "$WORK/dxc.exe" -nologo -E "$entry" -T "$target" \
      -Fo "$WORK/$output.cso" "$WORK/rov_dimensions.hlsl" >/dev/null; then
      ROV_DIMENSIONS_COMPILE_STATUS=1
      break
    fi
  done
fi

ROV_MSAA_COMPILE_STATUS=0
if [[ "$WITH_ROV_MSAA" == "1" ]]; then
  for spec in \
    "vs_main vs_6_0 rov_msaa_vs" \
    "ps_main ps_6_7 rov_msaa_ps" \
    "cs_main cs_6_7 rov_msaa_cs"; do
    read -r entry target output <<<"$spec"
    if ! env -u WINEDLLPATH -u DYLD_LIBRARY_PATH -u DXMT_WINEMETAL_UNIXLIB \
      -u DXMT_PROBE_D3D12_DLL -u DXMT_SHADER_CACHE_PATH -u DXMT_LOG_PATH \
      WINEDEBUG=-all WINEPREFIX="$PREFIX" \
      WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" "$WORK/dxc.exe" -nologo -E "$entry" -T "$target" \
      -Fo "$WORK/$output.cso" "$WORK/probe_rov_msaa.hlsl" >/dev/null; then
      ROV_MSAA_COMPILE_STATUS=1
      break
    fi
  done
fi

if [[ "$WITH_HOST_INVENTORY" == "1" ]]; then
  # Run the D3D12 feature-report probe only after the disposable runtime and
  # unique Winemetal alias have been installed.  Running it beside the native
  # Metal inventory above would resolve d3d12.dll through the host Wine tree.
  "$CXX" "${probe_flags[@]}" \
    "$SDK_DIR/probes/probe_device_caps/probe_device_caps.cpp" \
    -o "$WORK/probe_device_caps.exe"
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 30 --cwd "$WORK" \
    --output "$SANDBOX/device_caps.json" \
    --stderr "$LOG_DIR/probe-device-caps.stderr" -- \
    "$WINE_BIN" "$WORK/probe_device_caps.exe"
  CAPS_STATUS=$?
  set -e
fi

set +e
python3 "$BOUNDED_RUNNER" --timeout 60 --cwd "$WORK" \
  --output "$SANDBOX/interpolation.json" \
  --stderr "$LOG_DIR/probe-interpolation.stderr" -- \
  "$WINE_BIN" "$WORK/probe_interpolation.exe" \
  "$WORK/vs.cso" "$WORK/linear.cso" "$WORK/noperspective.cso" \
  "$WORK/centroid.cso" "$WORK/sample.cso" "$WORK/flat.cso" \
  "$WORK/evaluation.cso" "$WORK/invalid.cso"
INTERPOLATION_STATUS=$?
set -e

set +e
python3 "$BOUNDED_RUNNER" --timeout 30 --cwd "$WORK" \
  --output "$SANDBOX/invalid_descriptors.json" \
  --stderr "$LOG_DIR/probe-invalid-descriptors.stderr" -- \
  "$WINE_BIN" "$WORK/probe_phase6_invalid_descriptors.exe"
INVALID_DESCRIPTORS_STATUS=$?
set -e

RASTER_STATUS=0
if [[ "$WITH_RASTERIZATION" == "1" ]]; then
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 60 --cwd "$WORK" \
    --output "$SANDBOX/rasterization.json" \
    --stderr "$LOG_DIR/probe-rasterization.stderr" -- \
    "$WINE_BIN" "$WORK/probe_rasterization_breadth.exe"
  RASTER_STATUS=$?
  set -e
fi
ROV_DIMENSIONS_STATUS=0
if [[ "$WITH_ROV_DIMENSIONS" == "1" && "$ROV_DIMENSIONS_COMPILE_STATUS" == "0" ]]; then
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 60 --cwd "$WORK" \
    --output "$SANDBOX/rov_dimensions.json" \
    --stderr "$LOG_DIR/probe-rov-dimensions.stderr" -- \
    "$WINE_BIN" "$WORK/probe_rov_dimensions.exe" \
    "$WORK/rov_vs.cso" "$WORK/rov_1d.cso" \
    "$WORK/rov_1d_array.cso" "$WORK/rov_3d.cso"
  ROV_DIMENSIONS_STATUS=$?
  set -e
elif [[ "$WITH_ROV_DIMENSIONS" == "1" ]]; then
  ROV_DIMENSIONS_STATUS=1
fi
ROV_MSAA_STATUS=0
if [[ "$WITH_ROV_MSAA" == "1" && "$ROV_MSAA_COMPILE_STATUS" == "0" ]]; then
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 60 --cwd "$WORK" \
    --output "$SANDBOX/rov_msaa.json" \
    --stderr "$LOG_DIR/probe-rov-msaa.stderr" -- \
    "$WINE_BIN" "$WORK/probe_rov_msaa.exe" \
    "$WORK/rov_msaa_vs.cso" "$WORK/rov_msaa_ps.cso" \
    "$WORK/rov_msaa_cs.cso"
  ROV_MSAA_STATUS=$?
  set -e
elif [[ "$WITH_ROV_MSAA" == "1" ]]; then
  ROV_MSAA_STATUS=1
fi
SAMPLE_POSITIONS_STATUS=0
if [[ "$WITH_SAMPLE_POSITIONS" == "1" ]]; then
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 60 --cwd "$WORK" \
    --output "$SANDBOX/sample_positions.json" \
    --stderr "$LOG_DIR/probe-sample-positions.stderr" -- \
    "$WINE_BIN" "$WORK/probe_sample_positions_breadth.exe"
  SAMPLE_POSITIONS_STATUS=$?
  set -e
fi
VIEW_ID_COMPILE_STATUS=0
if [[ "$WITH_VIEW_INSTANCING" == "1" ]]; then
  for spec in \
    "vs_main vs_6_8 probe_view_id_instancing_vs" \
    "ps_main ps_6_0 probe_view_id_instancing_ps"; do
    read -r entry target output <<<"$spec"
    if ! env -u WINEDLLPATH -u DYLD_LIBRARY_PATH -u DXMT_WINEMETAL_UNIXLIB \
      -u DXMT_PROBE_D3D12_DLL -u DXMT_SHADER_CACHE_PATH -u DXMT_LOG_PATH \
      WINEDEBUG=-all WINEPREFIX="$PREFIX" \
      WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" "$WORK/dxc.exe" -nologo -E "$entry" -T "$target" \
      -Fo "$WORK/$output.cso" "$WORK/probe_view_id_instancing.hlsl" >/dev/null; then
      VIEW_ID_COMPILE_STATUS=1
      break
    fi
  done
fi
VIEW_INSTANCING_STATUS=0
VIEW_INSTANCING_MSAA_STATUS=0
if [[ "$WITH_VIEW_INSTANCING" == "1" ]]; then
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 60 --cwd "$WORK" \
    --output "$SANDBOX/view_instancing.json" \
    --stderr "$LOG_DIR/probe-view-instancing.stderr" -- \
    "$WINE_BIN" "$WORK/probe_view_instancing_breadth.exe" 1
  VIEW_INSTANCING_STATUS=$?
  set -e
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 60 --cwd "$WORK" \
    --output "$SANDBOX/view_instancing_msaa.json" \
    --stderr "$LOG_DIR/probe-view-instancing-msaa.stderr" -- \
    "$WINE_BIN" "$WORK/probe_view_instancing_breadth.exe" 4
  VIEW_INSTANCING_MSAA_STATUS=$?
  set -e
fi
VIEW_ID_STATUS=0
if [[ "$WITH_VIEW_INSTANCING" == "1" && "$VIEW_ID_COMPILE_STATUS" == "0" ]]; then
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 60 --cwd "$WORK" \
    --output "$SANDBOX/view_id.json" \
    --stderr "$LOG_DIR/probe-view-id.stderr" -- \
    "$WINE_BIN" "$WORK/probe_mini_view_id_instancing.exe"
  VIEW_ID_STATUS=$?
  set -e
elif [[ "$WITH_VIEW_INSTANCING" == "1" ]]; then
  VIEW_ID_STATUS=1
fi
FIXED_FUNCTION_STATUS=0
INNER_COVERAGE_STATUS=0
CONSERVATIVE_MSAA_STATUS=0
CONSERVATIVE_MSAA_2_STATUS=0
INDEPENDENT_LOGIC_STATUS=0
INDEPENDENT_LOGIC_2_STATUS=0
INDEPENDENT_LOGIC_4_STATUS=0
if [[ "$WITH_FIXED_FUNCTION" == "1" ]]; then
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 90 --cwd "$WORK" \
    --output "$SANDBOX/fixed_function.json" \
    --stderr "$LOG_DIR/probe-fixed-function.stderr" -- \
    "$WINE_BIN" "$WORK/probe_graphics_pso.exe"
  FIXED_FUNCTION_STATUS=$?
  set -e
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 60 --cwd "$WORK" \
    --output "$SANDBOX/inner_coverage.json" \
    --stderr "$LOG_DIR/probe-inner-coverage.stderr" -- \
    "$WINE_BIN" "$WORK/probe_mini_inner_coverage.exe"
  INNER_COVERAGE_STATUS=$?
  set -e
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 60 --cwd "$WORK" \
    --output "$SANDBOX/conservative_msaa.json" \
    --stderr "$LOG_DIR/probe-conservative-msaa.stderr" -- \
    "$WINE_BIN" "$WORK/probe_conservative_msaa.exe" \
    "$WORK/probe_conservative_raster_vs.cso" \
    "$WORK/probe_conservative_raster_ps.cso" 4
  CONSERVATIVE_MSAA_STATUS=$?
  set -e
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 60 --cwd "$WORK" \
    --output "$SANDBOX/conservative_msaa_2.json" \
    --stderr "$LOG_DIR/probe-conservative-msaa-2.stderr" -- \
    "$WINE_BIN" "$WORK/probe_conservative_msaa.exe" \
    "$WORK/probe_conservative_raster_vs.cso" \
    "$WORK/probe_conservative_raster_ps.cso" 2
  CONSERVATIVE_MSAA_2_STATUS=$?
  set -e
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 90 --cwd "$WORK" \
    --output "$SANDBOX/independent_logic.json" \
    --stderr "$LOG_DIR/probe-independent-logic.stderr" -- \
    "$WINE_BIN" "$WORK/probe_independent_logic_breadth.exe" 1
  INDEPENDENT_LOGIC_STATUS=$?
  set -e
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 90 --cwd "$WORK" \
    --output "$SANDBOX/independent_logic_2.json" \
    --stderr "$LOG_DIR/probe-independent-logic-2.stderr" -- \
    "$WINE_BIN" "$WORK/probe_independent_logic_breadth.exe" 2
  INDEPENDENT_LOGIC_2_STATUS=$?
  set -e
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 90 --cwd "$WORK" \
    --output "$SANDBOX/independent_logic_4.json" \
    --stderr "$LOG_DIR/probe-independent-logic-4.stderr" -- \
    "$WINE_BIN" "$WORK/probe_independent_logic_breadth.exe" 4
  INDEPENDENT_LOGIC_4_STATUS=$?
  set -e
fi
MSAA_STATUS=0
GRAPHICS_MSAA_STATUS=0
GRAPHICS_MSAA_DEPTH_STATUS=0
if [[ "$WITH_MSAA" == "1" ]]; then
  set +e
  python3 "$BOUNDED_RUNNER" --timeout "$MSAA_TIMEOUT_SECONDS" --cwd "$WORK" \
    --output "$SANDBOX/msaa.json" \
    --stderr "$LOG_DIR/probe-msaa.stderr" -- \
    "$WINE_BIN" "$WORK/probe_writable_msaa.exe"
  MSAA_STATUS=$?
  set -e
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 60 --cwd "$WORK" \
    --output "$SANDBOX/graphics_msaa.json" \
    --stderr "$LOG_DIR/probe-graphics-msaa.stderr" -- \
    "$WINE_BIN" "$WORK/probe_graphics_msaa_breadth.exe" \
    "$WORK/graphics_msaa_vs.cso" "$WORK/graphics_msaa_ps.cso"
  GRAPHICS_MSAA_STATUS=$?
  set -e
  set +e
  python3 "$BOUNDED_RUNNER" --timeout 60 --cwd "$WORK" \
    --output "$SANDBOX/graphics_msaa_depth.json" \
    --stderr "$LOG_DIR/probe-graphics-msaa-depth.stderr" -- \
    "$WINE_BIN" "$WORK/probe_graphics_msaa_depth.exe" \
    "$WORK/graphics_msaa_depth_vs.cso" "$WORK/graphics_msaa_depth_ps.cso"
  GRAPHICS_MSAA_DEPTH_STATUS=$?
  set -e
fi

python3 - "$SANDBOX/interpolation.json" "$STAGE_MANIFEST" "$ABI_RESULT" \
  "$RESULTS_DIR/phase6-exhaustive-$PROFILE.json" "$INTERPOLATION_STATUS" \
  "$INVALID_DESCRIPTORS_STATUS" "$SANDBOX/invalid_descriptors.json" \
  "$RASTER_STATUS" "$SANDBOX/rasterization.json" "$ROV_DIMENSIONS_STATUS" \
  "$ROV_DIMENSIONS_COMPILE_STATUS" "$SANDBOX/rov_dimensions.json" \
  "$ROV_MSAA_STATUS" "$ROV_MSAA_COMPILE_STATUS" "$SANDBOX/rov_msaa.json" \
  "$SAMPLE_POSITIONS_STATUS" "$SANDBOX/sample_positions.json" \
  "$VIEW_INSTANCING_STATUS" "$SANDBOX/view_instancing.json" \
  "$VIEW_INSTANCING_MSAA_STATUS" "$SANDBOX/view_instancing_msaa.json" \
  "$VIEW_ID_STATUS" "$VIEW_ID_COMPILE_STATUS" "$SANDBOX/view_id.json" \
  "$FIXED_FUNCTION_STATUS" "$SANDBOX/fixed_function.json" \
  "$INNER_COVERAGE_STATUS" "$SANDBOX/inner_coverage.json" \
  "$CONSERVATIVE_MSAA_STATUS" "$SANDBOX/conservative_msaa.json" \
  "$CONSERVATIVE_MSAA_2_STATUS" "$SANDBOX/conservative_msaa_2.json" \
  "$INDEPENDENT_LOGIC_STATUS" "$SANDBOX/independent_logic.json" \
  "$INDEPENDENT_LOGIC_2_STATUS" "$SANDBOX/independent_logic_2.json" \
  "$INDEPENDENT_LOGIC_4_STATUS" "$SANDBOX/independent_logic_4.json" \
  "$MSAA_STATUS" "$SANDBOX/msaa.json" "$GRAPHICS_MSAA_STATUS" \
  "$SANDBOX/graphics_msaa.json" "$GRAPHICS_MSAA_DEPTH_STATUS" \
  "$SANDBOX/graphics_msaa_depth.json" "$HOST_STATUS" \
  "$SANDBOX/host_inventory.json" "$CAPS_STATUS" "$SANDBOX/device_caps.json" <<'PY'
import json
import pathlib
import sys

(interpolation_path, stage_path, abi_path, output_path, interpolation_status,
 invalid_descriptors_status, invalid_descriptors_path, raster_status, raster_path,
 rov_status, rov_compile_status, rov_path,
 rov_msaa_status, rov_msaa_compile_status, rov_msaa_path,
 sample_positions_status, sample_positions_path,
 view_instancing_status, view_instancing_path,
 view_instancing_msaa_status, view_instancing_msaa_path,
 view_id_status, view_id_compile_status, view_id_path,
 fixed_function_status, fixed_function_path,
 inner_coverage_status, inner_coverage_path,
 conservative_msaa_status, conservative_msaa_path,
 conservative_msaa_2_status, conservative_msaa_2_path,
 independent_logic_status, independent_logic_path,
 independent_logic_2_status, independent_logic_2_path,
 independent_logic_4_status, independent_logic_4_path,
 msaa_status, msaa_path, graphics_msaa_status, graphics_msaa_path,
 graphics_msaa_depth_status, graphics_msaa_depth_path,
 host_status, host_path, caps_status, caps_path) = sys.argv[1:]
def load(path):
    p = pathlib.Path(path)
    if not p.exists() or not p.read_text(encoding="utf-8").strip():
        return {"parse_error": f"missing or empty result: {p}"}
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return {"parse_error": f"invalid JSON in {p}: {exc}"}

stage = load(stage_path)
abi = load(abi_path)
interpolation = load(interpolation_path)
payload = {
    "schema": "metalsharp.d3d12.phase6-exhaustive-result.v1",
    "profile": stage.get("profile", pathlib.Path(output_path).stem),
    "scope": "ordinary_graphics",
    "target": {
        "agility_sdk": "1.619.5",
        "wine": "11.5",
        "metal_device": "Apple M4",
        "metal_language": "Metal 4",
        "xcode": "27 beta 6",
        "llvm": "15",
        "metal_shader_converter": "/nonexistent",
    },
    "stage": stage,
    "abi": abi,
    "source_commit": stage.get("source_commit"),
    "source_tree_sha256": stage.get("source_tree_sha256"),
    "source_dirty": stage.get("source_dirty"),
    "bounded_timeout_seconds": 120,
    "interpolation": {"process_status": int(interpolation_status), "result": interpolation},
    "invalid_descriptors": None,
    "rasterization": None,
    "rov_dimensions": None,
    "rov_msaa": None,
    "sample_positions": None,
    "view_instancing": None,
    "view_instancing_msaa": None,
    "view_id_instancing": None,
    "fixed_function": None,
    "inner_coverage": None,
    "conservative_msaa": None,
    "conservative_msaa_2": None,
    "independent_logic": None,
    "independent_logic_2": None,
    "independent_logic_4": None,
    "msaa": None,
    "graphics_msaa": None,
    "graphics_msaa_depth": None,
    "host_inventory": None,
    "device_caps": None,
}
if pathlib.Path(raster_path).exists():
    payload["rasterization"] = {"process_status": int(raster_status), "result": load(raster_path)}
if pathlib.Path(invalid_descriptors_path).exists():
    payload["invalid_descriptors"] = {
        "process_status": int(invalid_descriptors_status),
        "result": load(invalid_descriptors_path),
    }
if pathlib.Path(rov_path).exists():
    payload["rov_dimensions"] = {
        "compile_status": int(rov_compile_status),
        "process_status": int(rov_status),
        "result": load(rov_path),
    }
if pathlib.Path(rov_msaa_path).exists():
    payload["rov_msaa"] = {
        "compile_status": int(rov_msaa_compile_status),
        "process_status": int(rov_msaa_status),
        "result": load(rov_msaa_path),
    }
if pathlib.Path(sample_positions_path).exists():
    payload["sample_positions"] = {
        "process_status": int(sample_positions_status),
        "result": load(sample_positions_path),
    }
if pathlib.Path(view_instancing_path).exists():
    payload["view_instancing"] = {
        "process_status": int(view_instancing_status),
        "result": load(view_instancing_path),
    }
if pathlib.Path(view_instancing_msaa_path).exists():
    payload["view_instancing_msaa"] = {
        "process_status": int(view_instancing_msaa_status),
        "result": load(view_instancing_msaa_path),
    }
if pathlib.Path(view_id_path).exists():
    payload["view_id_instancing"] = {
        "compile_status": int(view_id_compile_status),
        "process_status": int(view_id_status),
        "result": load(view_id_path),
    }
if pathlib.Path(fixed_function_path).exists():
    payload["fixed_function"] = {
        "process_status": int(fixed_function_status),
        "result": load(fixed_function_path),
    }
if pathlib.Path(inner_coverage_path).exists():
    payload["inner_coverage"] = {
        "process_status": int(inner_coverage_status),
        "result": load(inner_coverage_path),
    }
if pathlib.Path(conservative_msaa_path).exists():
    payload["conservative_msaa"] = {
        "process_status": int(conservative_msaa_status),
        "result": load(conservative_msaa_path),
    }
if pathlib.Path(conservative_msaa_2_path).exists():
    payload["conservative_msaa_2"] = {
        "process_status": int(conservative_msaa_2_status),
        "result": load(conservative_msaa_2_path),
    }
if pathlib.Path(independent_logic_path).exists():
    payload["independent_logic"] = {
        "process_status": int(independent_logic_status),
        "result": load(independent_logic_path),
    }
if pathlib.Path(independent_logic_2_path).exists():
    payload["independent_logic_2"] = {
        "process_status": int(independent_logic_2_status),
        "result": load(independent_logic_2_path),
    }
if pathlib.Path(independent_logic_4_path).exists():
    payload["independent_logic_4"] = {
        "process_status": int(independent_logic_4_status),
        "result": load(independent_logic_4_path),
    }
if pathlib.Path(msaa_path).exists():
    payload["msaa"] = {"process_status": int(msaa_status), "result": load(msaa_path)}
if pathlib.Path(graphics_msaa_path).exists():
    payload["graphics_msaa"] = {
        "process_status": int(graphics_msaa_status),
        "result": load(graphics_msaa_path),
    }
if pathlib.Path(graphics_msaa_depth_path).exists():
    payload["graphics_msaa_depth"] = {
        "process_status": int(graphics_msaa_depth_status),
        "result": load(graphics_msaa_depth_path),
    }
if pathlib.Path(host_path).exists():
    payload["host_inventory"] = {"process_status": int(host_status), "result": load(host_path)}
if pathlib.Path(caps_path).exists():
    payload["device_caps"] = {"process_status": int(caps_status), "result": load(caps_path)}
payload["exact"] = (
    bool(stage.get("ok"))
    and bool(abi.get("ok"))
    and int(interpolation_status) == 0
    and interpolation.get("exact") is True
    and payload["invalid_descriptors"] is not None
    and int(invalid_descriptors_status) == 0
    and payload["invalid_descriptors"]["result"].get("pass") is True
    and (payload["rasterization"] is None or (
        int(raster_status) == 0 and payload["rasterization"]["result"].get("pass") is True
    ))
    and (payload["rov_dimensions"] is None or (
        int(rov_compile_status) == 0
        and int(rov_status) == 0
        and payload["rov_dimensions"]["result"].get("exact") is True
    ))
    and (payload["rov_msaa"] is None or (
        int(rov_msaa_compile_status) == 0
        and int(rov_msaa_status) == 0
        and payload["rov_msaa"]["result"].get("pass") is True
    ))
    and (payload["sample_positions"] is None or (
        int(sample_positions_status) == 0
        and payload["sample_positions"]["result"].get("pass") is True
    ))
    and (payload["view_instancing"] is None or (
        int(view_instancing_status) == 0
        and payload["view_instancing"]["result"].get("pass") is True
    ))
    and (payload["view_instancing_msaa"] is None or (
        int(view_instancing_msaa_status) == 0
        and payload["view_instancing_msaa"]["result"].get("pass") is True
    ))
    and (payload["view_id_instancing"] is None or (
        int(view_id_compile_status) == 0
        and int(view_id_status) == 0
        and payload["view_id_instancing"]["result"].get("ok") is True
    ))
    and (payload["fixed_function"] is None or (
        int(fixed_function_status) == 0
        and payload["fixed_function"]["result"].get("pass") is True
    ))
    and (payload["inner_coverage"] is None or (
        int(inner_coverage_status) == 0
        and payload["inner_coverage"]["result"].get("ok") is True
    ))
    and (payload["conservative_msaa"] is None or (
        int(conservative_msaa_status) == 0
        and payload["conservative_msaa"]["result"].get("pass") is True
    ))
    and (payload["conservative_msaa_2"] is None or (
        int(conservative_msaa_2_status) == 0
        and payload["conservative_msaa_2"]["result"].get("pass") is True
    ))
    and (payload["independent_logic"] is None or (
        int(independent_logic_status) == 0
        and payload["independent_logic"]["result"].get("pass") is True
    ))
    and (payload["independent_logic_2"] is None or (
        int(independent_logic_2_status) == 0
        and payload["independent_logic_2"]["result"].get("pass") is True
    ))
    and (payload["independent_logic_4"] is None or (
        int(independent_logic_4_status) == 0
        and payload["independent_logic_4"]["result"].get("pass") is True
    ))
    and (payload["msaa"] is None or (
        int(msaa_status) == 0
        and payload["msaa"]["result"].get("pass") is True
    ))
    and (payload["graphics_msaa"] is None or (
        int(graphics_msaa_status) == 0
        and payload["graphics_msaa"]["result"].get("pass") is True
    ))
    and (payload["graphics_msaa_depth"] is None or (
        int(graphics_msaa_depth_status) == 0
        and payload["graphics_msaa_depth"]["result"].get("pass") is True
    ))
    and (payload["host_inventory"] is None or (
        int(host_status) == 0
        and payload["host_inventory"]["result"].get("exact") is True
    ))
    and (payload["device_caps"] is None or (
        int(caps_status) == 0
        and payload["device_caps"]["result"].get("pass") is True
    ))
)
pathlib.Path(output_path).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(output_path)
PY

if [[ "$INTERPOLATION_STATUS" != "0" || "$INVALID_DESCRIPTORS_STATUS" != "0" || "$RASTER_STATUS" != "0" || "$ROV_DIMENSIONS_STATUS" != "0" || "$ROV_MSAA_STATUS" != "0" || "$SAMPLE_POSITIONS_STATUS" != "0" || "$VIEW_INSTANCING_STATUS" != "0" || "$VIEW_INSTANCING_MSAA_STATUS" != "0" || "$VIEW_ID_STATUS" != "0" || "$FIXED_FUNCTION_STATUS" != "0" || "$INNER_COVERAGE_STATUS" != "0" || "$CONSERVATIVE_MSAA_STATUS" != "0" || "$CONSERVATIVE_MSAA_2_STATUS" != "0" || "$INDEPENDENT_LOGIC_STATUS" != "0" || "$INDEPENDENT_LOGIC_2_STATUS" != "0" || "$INDEPENDENT_LOGIC_4_STATUS" != "0" || "$MSAA_STATUS" != "0" || "$GRAPHICS_MSAA_STATUS" != "0" || "$GRAPHICS_MSAA_DEPTH_STATUS" != "0" || "$HOST_STATUS" != "0" || "$CAPS_STATUS" != "0" ]]; then
  echo "[FAIL] Phase 6 probe process failed (interpolation=$INTERPOLATION_STATUS invalid_descriptors=$INVALID_DESCRIPTORS_STATUS rasterization=$RASTER_STATUS rov_dimensions=$ROV_DIMENSIONS_STATUS rov_msaa=$ROV_MSAA_STATUS sample_positions=$SAMPLE_POSITIONS_STATUS view_instancing=$VIEW_INSTANCING_STATUS view_instancing_msaa=$VIEW_INSTANCING_MSAA_STATUS view_id=$VIEW_ID_STATUS fixed_function=$FIXED_FUNCTION_STATUS inner_coverage=$INNER_COVERAGE_STATUS conservative_msaa=$CONSERVATIVE_MSAA_STATUS conservative_msaa_2=$CONSERVATIVE_MSAA_2_STATUS independent_logic=$INDEPENDENT_LOGIC_STATUS independent_logic_2=$INDEPENDENT_LOGIC_2_STATUS independent_logic_4=$INDEPENDENT_LOGIC_4_STATUS writable_msaa=$MSAA_STATUS graphics_msaa=$GRAPHICS_MSAA_STATUS graphics_msaa_depth=$GRAPHICS_MSAA_DEPTH_STATUS host_inventory=$HOST_STATUS device_caps=$CAPS_STATUS)" >&2
  exit 1
fi
if ! python3 "$SDK_DIR/scripts/validate-phase6-exhaustive.py" \
  --result "$RESULTS_DIR/phase6-exhaustive-$PROFILE.json" >/dev/null; then
  echo "[FAIL] exhaustive manifest/result validation failed" >&2
  exit 1
fi
printf '[PASS] Phase 6 exhaustive provider smoke gate: %s\n' "$RESULTS_DIR/phase6-exhaustive-$PROFILE.json"
