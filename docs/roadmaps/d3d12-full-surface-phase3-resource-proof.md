# Full-Surface Phase 3 Resource, Residency, and Sharing Proof

**Status:** Phase 3 resource milestone; full phase remains open
**Stable runtime:** MetalSharp Wine 11.5 / Apple M4 / Metal 4
**Stable Agility lane:** 1.619.5 (`D3D12SDKVersion=619`)

## Implemented behavior

- CPU-visible upload/readback buffers and default-resource
  `ReadFromSubresource`/`WriteToSubresource` now use validated buffer copies.
- Texture subresource I/O resolves mip, array, box, block-compressed, and
  volume regions with correct row and slice pitches.
- Resource and heap residency state tracks resident/evicted transitions and
  priority; mapping an evicted resource is rejected until it is made resident
  again.
- Named buffer sharing uses a platform file mapping with a fixed metadata
  header rather than a process-local object map. A second Wine process can
  reconstruct the D3D12 resource and observe writes to the same backing.
- Resource lifetime, placed-resource aliasing, reserved buffers/textures,
  packed and partial mips, volume tiling, format variants, tile mapping,
  unmapped zeroing, and physical-page ownership remain behavior-backed by the
  resource probe.
- Shared mapping views and section handles are released with the resource;
  malformed or unknown handles remain rejected.

## Exact evidence

### Resource, sparse, residency, and cross-process sharing

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --resources-only
```

The isolated source-staged probe passed with:

```json
{
  "pass": true,
  "buffers.default_cpu_io_verified": true,
  "buffers.residency_state_verified": true,
  "shared_handles.roundtrip_verified": true,
  "shared_handles.cross_process_verified": true,
  "textures.unaligned_bc1_copy_verified": true,
  "sparse.unmapped_zero_verified": true,
  "sparse.tier3_physical_page_ownership_verified": true,
  "sparse.volume_copy_verified": true,
  "sparse.volume_alias_copy_verified": true
}
```

The same result records exact successful creation, queue execution, signal,
wait, map, copy, readback, residency, and invalid-handle HRESULTs. The child
process is bounded to 30 seconds and the source wrapper removes its disposable
Wine clone and prefix on every exit path.

### D3D10/D3D11 regression

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --legacy-regression-only
```

The legacy regression probe passed after the resource and backing changes.

### Build, ABI, and cleanup

- Clean DXMT source build passed **159/159** targets.
- Source-staged resource and legacy probes passed in disposable Wine 11.5
  clones.
- The source wrapper removed every isolated prefix and Wine clone used by the
  evidence commands.
- Generated build products, probe caches, and temporary staging data were
  removed before delivery; no runtime binaries are committed.

This checkpoint completes the behavior-backed resource, sparse-resource,
residency, and buffer-sharing subset. The full Phase 3 exit gate remains open
for heap/file-mapping reopening, shared heaps/fences/events, and exhaustive
legal-shape coverage; those items remain explicitly ledgered rather than
promoted by query results alone.
