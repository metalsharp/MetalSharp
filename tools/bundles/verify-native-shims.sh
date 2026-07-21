#!/bin/bash
set -euo pipefail

# Verifies that the MetalSharp native shim dylibs are real Mach-O binaries,
# not zero-byte placeholders left behind by a stale
# `tools/package/prepare-native-placeholders.sh` run. Intended for CI gating
# after the split-bundle pipeline produces metalsharp-scripts-tools.tar.zst,
# but also runnable against app/native/ during local development.
#
# Usage:
#   tools/bundles/verify-native-shims.sh [NATIVE_DIR]
#
# If NATIVE_DIR is not given, uses $METALSHARP_NATIVE_DIR or app/native.

NATIVE_DIR="${1:-${METALSHARP_NATIVE_DIR:-app/native}}"

required_dylibs=(
    d3d11.dylib d3d12.dylib dxgi.dylib xaudio2_9.dylib xinput1_4.dylib opengl32.dylib
)
required_bins=(metalsharp metalsharp_launcher)

errors=0
for f in "${required_dylibs[@]}" "${required_bins[@]}"; do
    path="$NATIVE_DIR/$f"
    if [ ! -f "$path" ]; then
        echo "ERROR: missing native shim: $path"
        errors=$((errors + 1))
    elif [ ! -s "$path" ]; then
        echo "ERROR: zero-byte native shim (placeholder not replaced): $path"
        errors=$((errors + 1))
    else
        echo "OK: $path ($(wc -c < "$path") bytes)"
        # For dylibs, verify they're Mach-O binaries
        if [[ "$f" == *.dylib ]]; then
            if ! file "$path" | grep -q "Mach-O"; then
                echo "ERROR: $path is not a Mach-O binary"
                errors=$((errors + 1))
            fi
            # Verify key symbols are exported
            if [[ "$f" == "xinput1_4.dylib" ]]; then
                for sym in XInputGetState XInputSetState XInputGetCapabilities; do
                    if ! nm "$path" 2>/dev/null | grep -q "T _$sym"; then
                        echo "ERROR: $sym not exported from $path"
                        errors=$((errors + 1))
                    fi
                done
            fi
        fi
    fi
done

if [ $errors -gt 0 ]; then
    echo "FAILED: $errors validation errors"
    exit 1
fi
echo "PASSED: all native shims present and valid"