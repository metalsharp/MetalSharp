# Node metadata ownership regression

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
empty input, and rejection of missing/duplicate entrypoints. It checks module-copy ownership
and null, missing, and out-of-range operand lookup. It fails against the prior
parser because `dx.entryPoints` is discarded. This does not certify arbitrary
malformed bitcode rejection or implement runtime record allocation.
