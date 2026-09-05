# Node metadata ownership regression

## Constant-expression GEP regression

For `test_constant_gep.cpp`, use the commands below with
`node_chain.hlsl` instead of `node_input_records.hlsl`, omit
`-enable-16bit-types`, and compile/run `test_constant_gep.cpp` instead of
`test_node_metadata.cpp`. Keep the same DXIL-container extraction step.
The test requires typed constant GEP operands to retain the groupshared global
base and four distinct indices (byte offsets 0, 4, 8, 12); fabricated null
constants fail. Runtime reduction coverage is the SDK's
`probe_workgraph_chain.exe`, included in `run-probes.sh --work-graph-only`.

## Grid semantic regression

Compile `node_chain.hlsl` to DXIL and extract raw bitcode as below. Build
`test_grid_metadata.cpp` with `llvm_bitcode.cpp` and the same host flags.
Pass the raw bitcode path followed by expected offset, component byte width,
and component count:

| DXC defines/options | Expected arguments |
| --- | --- |
| none | `0 4 1` |
| `-DGRID_OFFSET=1` | `4 4 1` |
| `-DGRID_VECTOR=1` | `4 4 3` |
| `-DGRID_U16=1 -enable-16bit-types` | `2 2 3` |

The runtime chain probe verifies the resulting GPU dispatch grids separately.

## Input metadata regression

This is a host parser test, not a Work Graph execution or scheduling gate.
Run from the repository root with an initialized disposable `WINEPREFIX`, a
matching Wine executable in `$WINE`, and the pinned DXC already downloaded.
All generated artifacts go to `$tmp` outside the repository.

```sh
tmp=$(mktemp -d)
export tmp
"$WINE" tools/d3d12-metal-sdk/cache/dxc/v1.9.2602/bin/x64/dxc.exe \
  -T lib_6_8 -enable-16bit-types -Fo "$tmp/node.cso" \
  tools/d3d12-metal-sdk/probes/probe_workgraph/node_input_records.hlsl
python3 - <<'PY'
import os, pathlib, struct
root = pathlib.Path(os.environ['tmp'])
b = (root / 'node.cso').read_bytes()
assert b[:4] == b'DXBC'
for i in range(struct.unpack_from('<I', b, 28)[0]):
    chunk = struct.unpack_from('<I', b, 32 + i * 4)[0]
    if b[chunk:chunk + 4] != b'DXIL':
        continue
    header = chunk + 16  # chunk header + program header
    offset, size = struct.unpack_from('<II', b, header + 8)
    bc = b[header + offset:header + offset + size]
    assert len(bc) == size and bc[:4] == b'BC\xc0\xde'
    (root / 'node.bc').write_bytes(bc)
    break
else:
    raise RuntimeError('DXIL chunk missing')
PY
clang++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-const-variable \
  -Ivendor/dxmt/src/airconv/dxil \
  vendor/dxmt/tests/dxil/test_node_metadata.cpp \
  vendor/dxmt/src/airconv/dxil/llvm_bitcode.cpp \
  -o "$tmp/test-node-metadata"
DXMT_LOG_PATH="$tmp" "$tmp/test-node-metadata" "$tmp/node.bc"
```

The warning exception is for an existing unused parser constant. The test
checks distinct entrypoint record sizes (4 and 16), alignment (4), threadgroup
sizes (1 and 4), and dispatch grids (1 and 2). It also checks the production
layout decoder's DWORD padding of a 16-bit record, zero size/alignment for
empty input, and rejection of missing/duplicate entrypoints. The launch decoder
checks fixed per-entrypoint threadgroup/grid values and refuses to substitute a
max grid for record-driven SV_DispatchGrid. The vector GEP check verifies that
all three indices survive bitcode parsing, including the dynamic lane index.
It checks module-copy ownership
and null, missing, and out-of-range operand lookup. It fails against the prior
parser because `dx.entryPoints` is discarded. The padded mixed-width fixture additionally checks GEP source-type retention,
root member offsets `[0,16]`, nested-tail offsets `[0,8,16]`, 48-byte total
size and 8-byte alignment (absolute field offsets `[0,16,24,32]`). The test
also retains a coalescing input's `[MaxRecords(4)]` metadata and launch type;
the runtime uses that limit for GPU batch boundaries. Packed types,
invalid type IDs, recursive layouts and multiplication overflow are rejected by
the node layout helper. This does not certify arbitrary malformed bitcode
rejection or implement runtime record allocation.
