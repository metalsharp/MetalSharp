#!/bin/bash
# Required Notice: Copyright (c) 2026 MetalSharp. Commercial licensing: averyfelts@aol.com
#
# launch-env-probe.sh — dump the host's wine-related environment and binaries.
#
# Use this on a machine where Steam fails to launch while CrossOver,
# SakuraGiri, Whisky, or another Wine launcher is installed. Run it once
# with the foreign launcher installed and (optionally) once after
# uninstalling it. The output shows which channel a foreign wine could
# leak into MetalSharp's launch chain:
#   1. wine binaries on PATH / /usr/local/bin (CrossOver installs symlinks there)
#   2. ambient WINE*/DYLD_* env vars the MetalSharp backend would inherit
#   3. the MetalSharp runtime wrapper and which wine it resolves to
set -u

echo "== host =="
uname -srm
echo

echo "== wine binaries on PATH =="
for bin in wine wine64 wineserver wineloader wineboot; do
  if command -v "$bin" >/dev/null 2>&1; then
    command -v "$bin"
    readlink "$(command -v "$bin")" 2>/dev/null | sed 's/^/  -> /'
  fi
done
echo "(end wine PATH scan)"
echo

echo "== /usr/local/bin wine/crossover symlinks =="
if ls -la /usr/local/bin/ 2>/dev/null | grep -iE "wine|crossover|\bcx\b"; then
  :
else
  echo "(none)"
fi
echo

echo "== wine-like apps in /Applications and ~/Applications =="
for app in /Applications/*CrossOver* /Applications/*Sakura* /Applications/*Whisky* \
           /Applications/*Porting* ~/Applications/*CrossOver* ~/Applications/*Sakura* \
           ~/Applications/*Whisky*; do
  [ -e "$app" ] && echo "$app"
done
echo "(end app scan)"
echo

echo "== ambient env (WINE*/CX_*/DYLD*) =="
env | grep -iE "^(WINE|CX_|DYLD|MTL_|MVK_|VK_)" || echo "(clean)"
echo

echo "== MetalSharp runtime =="
MS_WRAPPER="${METALSHARP_HOME:-$HOME/.metalsharp}/runtime/wine/bin/metalsharp-wine"
if [ -e "$MS_WRAPPER" ]; then
  ls -la "$MS_WRAPPER"
  file "$MS_WRAPPER" 2>/dev/null | head -1
  echo "-- wrapper resolves to:"
  MS_WINE_DEBUG_TRACE=1 "$MS_WRAPPER" --version 2>&1 | head -3 || true
else
  echo "wrapper not present at: $MS_WRAPPER"
fi
echo

echo "== wineserver sockets (per-prefix, informational) =="
ls -d /tmp/.wine-* 2>/dev/null | head -5 || echo "(none)"
echo

echo "== probe complete =="
