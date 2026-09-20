#!/usr/bin/env bash
# Required Notice: Copyright (c) 2026 MetalSharp. Commercial licensing: averyfelts@aol.com
#
# Assemble a faithful installed MetalSharp tree from the published bundle
# archives (the `bundles` release tag, or whatever METALSHARP_BUNDLE_DIR points
# at) exactly the way the installer does, then assert migration.c's
# runtime_ready() accepts it. Proves the pinned migration hashes/manifest
# contract can never present a false "runtime bundle is incomplete" error for
# healthy installs.
#
# Exit codes: 0 = runtime_ready accepted the tree, 1 = it did not (failure),
# 97 = skipped because the bundle archives are not present (CI make-test runs
# before the bundle-download step).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUNDLE_DIR="${METALSHARP_BUNDLE_DIR:-$ROOT_DIR/app/bundles}"
SIM_BIN="$1"

RUNTIME="$BUNDLE_DIR/metalsharp-runtime.tar.zst"
GRAPHICS="$BUNDLE_DIR/metalsharp-graphics-dll.tar.zst"
GOLDBERG="$BUNDLE_DIR/goldberg.tar.zst"
SCRIPTS="$BUNDLE_DIR/metalsharp-scripts-tools.tar.zst"

if [ ! -s "$RUNTIME" ] || [ ! -s "$GRAPHICS" ] || [ ! -s "$GOLDBERG" ] || [ ! -s "$SCRIPTS" ]; then
  echo "runtime-ready sim skipped (bundle archives not present in $BUNDLE_DIR)"
  exit 97
fi

SIM="$(mktemp -d "${TMPDIR:-/tmp}/ms-runtime-ready-sim.XXXXXX")"
trap 'rm -rf "$SIM"' EXIT

# 1. Runtime archive: wine, host runtime, MoltenVK, ICDs, dxmt.conf.
mkdir -p "$SIM"
tar --use-compress-program=unzstd -xf "$RUNTIME" -C "$SIM"

# 2. Graphics archive: stage the three lanes like install step 11
#    (Graphics/dll/dxmt -> runtime/wine/lib/dxmt, dxvk -> vkd3d/dxvk,
#    vkd3d-proton -> vkd3d/vkd3d-proton).
mkdir -p "$SIM/gfx" "$SIM/runtime/goldberg" "$SIM/configs" "$SIM/st" "$SIM/vkd3d"
tar --use-compress-program=unzstd -xf "$GRAPHICS" -C "$SIM/gfx"
tar --use-compress-program=unzstd -xf "$GOLDBERG" -C "$SIM/runtime/goldberg"
tar --use-compress-program=unzstd -xf "$SCRIPTS" -C "$SIM/st"
mkdir -p "$SIM/runtime/wine/lib/dxmt" && cp -R "$SIM/gfx/Graphics/dll/dxmt/." "$SIM/runtime/wine/lib/dxmt/"
mkdir -p "$SIM/vkd3d/dxvk" && cp -R "$SIM/gfx/Graphics/dll/dxvk/." "$SIM/vkd3d/dxvk/"
mkdir -p "$SIM/vkd3d/vkd3d-proton" && cp -R "$SIM/gfx/Graphics/dll/vkd3d-proton/." "$SIM/vkd3d/vkd3d-proton/"
cp "$SIM/st/scripts/tools/configs/mtsp-rules.toml" "$SIM/configs/mtsp-rules.toml"

# 3. Installer post-processing:
#    - ad-hoc re-sign the DXMT bridge (non-deterministic bytes)
#    - rewrite the runtime MoltenVK ICD library_path to the absolute dylib
codesign --force --sign - "$SIM/runtime/wine/lib/dxmt/x86_64-unix/winemetal.so"
python3 - "$SIM/runtime/wine/etc/vulkan/icd.d/MoltenVK_icd.json" \
  "$SIM/runtime/wine/lib/wine/x86_64-unix/libMoltenVK.dylib" <<'PY'
import json
import sys

icd_path, library = sys.argv[1], sys.argv[2]
with open(icd_path) as fh:
    data = json.load(fh)
data["ICD"]["library_path"] = library
with open(icd_path, "w") as fh:
    json.dump(data, fh, indent=4)
PY

# 4. DXMT manifest is written by the harness with this build's MIGRATION_VERSION.
"$SIM_BIN" write-manifest "$SIM/runtime/wine/lib/dxmt"

# 5. The production verdict.
"$SIM_BIN" check "$SIM"
