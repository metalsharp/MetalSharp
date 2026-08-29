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
- Named buffers, CPU-visible heaps, and fences use platform file mappings with
  fixed metadata headers rather than a process-local object map. A second Wine
  process can reconstruct a buffer, observe writes to the same backing, and
  observe a signaled fence value.
- `OpenExistingHeapFromAddress` resolves a live mapped heap only for the owning
  device, and `OpenExistingHeapFromFileMapping` validates and reconstructs a
  shared CPU-visible heap.
- Resource lifetime, placed-resource aliasing, reserved buffers/textures,
  packed and partial mips, volume tiling, format variants, tile mapping,
  unmapped zeroing, and physical-page ownership remain behavior-backed by the
  resource probe.
- Copyable footprint dimensions, pitches, row counts, and 512-byte placement
  alignment are validated for 1D, arrays, mip chains, volumes, and unaligned
  BC1 dimensions; BC footprints report texel dimensions while retaining
  block-rounded row math.
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
  "shared_handles.heap_roundtrip_verified": true,
  "shared_handles.heap_cross_process_verified": true,
  "shared_handles.fence_cross_process_verified": true,
  "buffers.address_heap_open_verified": true,
  "buffers.heap_aliasing_verified": true,
  "resource_shapes.all_created_and_roundtripped": true,
  "resource_shapes.footprint_matrix_verified": true,
  "resource_shapes.invalid_zero_width": "0x80070057",
  "resource_shapes.invalid_zero_width_allocation": [0, 0],
  "resource_shapes.invalid_msaa_mips": "0x80070057",
  "resource_shapes.invalid_msaa_mips_allocation": [0, 0],
  "resource_shapes.misaligned_placement": "0x80070057",
  "resource_shapes.invalid_heap_alignment": "0x80070057",
  "resource_shapes.invalid_heap_flags": "0x80070057",
  "textures.unaligned_bc1_copy_verified": true,
  "formats.D24_UNORM_S8_UINT.plane_count": 2,
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

### DXGI residency view

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --queues-only
```

The queue probe's `IDXGIDevice3::QueryResourceResidency` path observed
`DXGI_RESIDENCY_FULLY_RESIDENT (1)`, `DXGI_RESIDENCY_EVICTED_TO_DISK (3)`,
and `FULLY_RESIDENT (1)` around a real D3D12 `Evict`/`MakeResident` cycle.

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
residency, sharing, legal-shape creation, and malformed-descriptor validation
subset. The full Phase 3 exit gate remains open for exhaustive allocation and
footprint/plane behavior, every legal sparse tier and aliasing case, real
reclaim/trim behavior, and shared-event/security/LUID coverage; those items
remain explicitly ledgered rather than promoted by query results alone.
