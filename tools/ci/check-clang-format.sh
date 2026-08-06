#!/usr/bin/env bash
# Required Notice: Copyright (c) 2026 MetalSharp. Commercial licensing: averyfelts@aol.com
#
# Single source of truth for the C/C++/Obj-C clang-format scan. Both
# workflows (ci.yml, pr-ci.yml) invoke this so the exclusion set can never
# diverge between main and PR CI.
#
# Exclusions (union of both historical sets):
#   - vendored trees (pr-ci's */vendor/*) — not ours to format
#   - third-party Wine sources (pr-ci's */wine/*), EXCEPT our own
#     src/wine overrides (metalsharp_d3d11_pe.cpp etc.), which ci.yml
#     checked and which must stay checked
#   - generated D3D12 Metal SDK cache (ci.yml's tools/d3d12-metal-sdk/cache/*)
set -euo pipefail

cd "$(dirname "$0")/../.."

find src include tests tools \
  \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.mm' \) \
  -not -path '*/vendor/*' \
  -not -path 'src/wine/mojoshader/*' \
  -not -path 'tools/d3d12-metal-sdk/cache/*' \
  -print0 | xargs -0 clang-format --dry-run --Werror
