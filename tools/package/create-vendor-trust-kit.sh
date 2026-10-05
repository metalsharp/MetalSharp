#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${METALSHARP_VENDOR_KIT_DIR:-"$ROOT_DIR/dist/vendor-trust-kit"}"

mkdir -p "$OUT_DIR/docs"

copy_doc() {
    local doc="$1"
    if [[ -f "$ROOT_DIR/$doc" ]]; then
        cp "$ROOT_DIR/$doc" "$OUT_DIR/docs/"
    fi
}

copy_doc "README.md"
copy_doc "docs/research/anti-cheat-compatibility-boundaries.md"
copy_doc "docs/research/proton-runtime-research.md"
copy_doc "docs/architecture.md"

VERSION="$(sed -n 's/^project(metalsharp VERSION \([0-9][0-9.]*\) .*/\1/p' "$ROOT_DIR/CMakeLists.txt" | head -1)"
VERSION="${VERSION:-unknown}"
COMMIT="$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
CREATED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

cat > "$OUT_DIR/manifest.json" <<JSON
{
  "name": "MetalSharp Vendor Trust Kit",
  "status": "phase_9_foundation",
  "version": "$VERSION",
  "commit": "$COMMIT",
  "created_at": "$CREATED_AT",
  "policy": {
    "bypass": false,
    "spoof_kernel_trust": false,
    "vendor_enablement_required": true
  },
  "included_docs": [
    "README.md",
    "anti-cheat-compatibility-boundaries.md",
    "proton-runtime-research.md",
    "host-runtime-abi.md",
    "compatdata-architecture.md",
    "launcher-runtime.md",
    "redistributable-runtime.md",
    "darwin-sync-map.md",
    "steam-compatibility-tool-surface.md",
    "vendor-trust-kit.md"
  ],
  "required_external_evidence": [
    "signed_notarized_runtime_artifacts",
    "target_game_launch_logs",
    "launch_doctor_anticheat_classification_json",
    "process_tree",
    "vendor_contact_context"
  ]
}
JSON

printf 'Vendor trust kit staged at %s\n' "$OUT_DIR"
