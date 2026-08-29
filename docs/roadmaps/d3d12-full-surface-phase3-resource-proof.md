# Full-Surface Phase 3 Resource, Residency, and Sharing Proof

**Status:** Phase 3 resource milestone; full phase remains open
**Stable runtime:** MetalSharp Wine 11.5 / Apple M4 / Metal 4
**Stable Agility lane:** 1.619.5 (`D3D12SDKVersion=619`)

## Implemented behavior

- CPU-visible upload/readback buffers and default-resource
  `ReadFromSubresource`/`WriteToSubresource` now use validated buffer copies.
- Texture subresource I/O resolves mip, array, box, block-compressed, and
  volume regions with correct row and slice pitches.
- Resource and heap residency state is reference-counted as required by
  D3D12: after one extra `MakeResident`, the first `Evict` leaves mapping
  available, while the matching second `Evict` rejects `Map`; the state then
  remakes cleanly. It also propagates an explicit heap eviction to placed
  buffers: access is rejected while the heap is evicted and succeeds again
  after the heap is made resident. Resource priority is retained, and mapping
  an evicted resource is rejected until it is made resident again.
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
- Texture `GetGPUVirtualAddress` correctly returns zero; texture descriptor
  identity continues to use the Metal texture/resource id rather than a fake
  buffer address.
- Depth-stencil flags require a depth/stencil-compatible resource format;
  an R8 color texture with `ALLOW_DEPTH_STENCIL` rejects with
  `E_INVALIDARG` rather than creating a non-depth backing.
- Heap properties enforce the documented custom/non-custom CPU-page and
  memory-pool combinations and node-mask relationship; invalid combinations
  reject before allocation with exact `E_INVALIDARG`.
- Upload resources reject an initial `COPY_DEST` state and readback resources
  reject an initial `GENERIC_READ` state with exact `E_INVALIDARG`; only the
  documented heap-specific initial states are accepted.
- Optimized clear values are accepted only for matching render-target or
  depth-stencil resources; a clear value on a buffer and a depth clear format
  on a color target both reject with `E_INVALIDARG`.
- Resource flag validation rejects render-target resources with a buffer
  dimension and rejects mutually exclusive render-target/depth-stencil flags,
  both with exact `E_INVALIDARG` and no returned object.
- Heap restriction masks now distinguish the composite `ALLOW_ONLY_*` values
  from their individual deny bits, so `ALLOW_ONLY_NON_RT_DS_TEXTURES` accepts
  valid placed color resources while still rejecting buffers and RT/DS objects.
- The focused `ID3D12Device10` relaxed-cast lane accepts declared same-sized
  `R32_UINT` and `R8G8B8A8_UINT` views of an `R32_FLOAT` resource, rejects an
  undeclared `R32_SINT` view and an invalid castable list, and reads back the
  exact float bit pattern through committed, placed, and overlapping-alias
  resources.
- Direct BC1 subresource I/O now uses block-row counts for slice pitches:
  unaligned `7x5` BC1 write/read accepts two 16-byte rows and round-trips the
  exact compressed bytes, in addition to the command-list copy proof.
- Direct default-resource subresource I/O is behavior-backed for a 2D array
  with mips (boxed mip write/read) and a 3D volume (two depth slices), with
  exact row and slice pitches and byte readback.
- A two-slice 1D-array resource and a six-face cube resource (the D3D12
  six-slice 2D representation) create successfully; the backend preserves the
  1D array length, and each 1D-array slice and cube face independently
  round-trips distinct bytes through `WriteToSubresource`/`ReadFromSubresource`.
- NV12 footprints expand luma/chroma into R8/R8G8 planes with half-resolution
  chroma dimensions (`13x8` plus `7x4`, 3072 bytes total); odd NV12 heights
  reject with `E_INVALIDARG`.
- Planar depth/stencil footprints expand the D24/R24G8 family into the
  documented R32 depth and R8 stencil planes across array slices and mips;
  eight subresources report exact plane formats, row sizes, dimensions, and
  ordered offsets (`12032` total bytes). D24 plane-0 and plane-1 direct I/O
  round-trips exact depth/stencil bytes, and a queued stencil-only clear is
  visible through a subsequent plane-1 readback.
- Invalid `GetCopyableFootprints` descriptors initialize the documented
  `pTotalBytes` sentinel (`UINT64_MAX`) instead of reporting a fabricated
  zero-sized layout.
- Allocation-info and sideband allocation-info calls with a nonzero count and
  null descriptor arrays initialize `[SizeInBytes=0, Alignment=0]` and zero
  sideband output without dereferencing the caller's null pointer.
- Committed-resource validation rejects a null heap-properties pointer and a
  buffer placed in a heap with `DENY_BUFFERS`, both with exact
  `E_INVALIDARG`, before allocating any object.
- `ID3D12Device::GetAdapterLuid` returns a stable nonzero adapter identity in
  the same source-staged run as the resource/share tests; the existing DXGI
  factory lane cross-checks that identity against adapter enumeration.
- Named mapping handles no longer retain process-global COM references; each
  `OpenSharedHandleByName` call opens and validates a fresh file-mapping handle,
  while `OpenSharedHandle` reconstructs named objects first and uses the legacy
  registry only for unsupported unnamed object kinds.
- `OpenSharedHandle` reconstructs named mapping-backed resources as
  independent COM objects (rather than returning a process-global registry
  pointer); descriptor identity and shared bytes remain valid across the
  original and both opened objects.
- Shared-handle creation and named opening reject zero access masks with
  exact `E_INVALIDARG`; valid named buffer, heap, and fence mappings retain
  their cross-process behavior.
- Descriptor heaps and query heaps now participate in the D3D12 residency
  object set; the probe accepts and re-makes both pageables and applies their
  residency priorities without silently ignoring them.
- `EnqueueMakeResident` validates residency flags, makes the evicted resource
  resident, and signals a fence at value 9; an unknown flag is rejected with
  `E_INVALIDARG`.
- `CREATE_NOT_RESIDENT` is honored for committed and placed upload buffers:
  initial `Map` returns `DXGI_ERROR_INVALID_CALL`, `MakeResident` succeeds, and
  the remapped resources expose their CPU backing and exact byte roundtrip.
- `GetResourceTiling` honors partial query windows: requesting four entries
  from subresource 2 returns exactly two entries (`mip2` and the packed-tail
  marker) rather than claiming the full resource count.
- Reserved 512x512 RGBA8 resources with four mips now create successfully and
  report the packed tail exactly: 22 total tiles, three standard mips, one
  packed mip tile, and per-subresource starts `[0, 16, 20, D3D12_PACKED_TILE]`.
- Zero `MipLevels` is normalized to the complete mip chain: a 32x16 texture
  creates six mips and its six copy footprints report exact dimensions, row
  counts, row sizes, and increasing placement offsets.
- Stable tight alignment Tier 1 is behavior-backed for buffers: a committed
  1000-byte buffer reports `[SizeInBytes=1024, Alignment=256]`, a 256-byte
  placed upload buffer round-trips 1000 exact bytes, non-power-of-two and
  over-aligned placed requests reject, and reserved tight buffers reject.
- 3D allocation sizing counts volume depth once (a 64x64x4 R8 volume reports
  one 64-KiB allocation with 64-KiB alignment), while arrays continue to count
  `DepthOrArraySize` as slices.
- Reserved-buffer tile mappings cover all four D3D12 range modes. A dedicated
  one-tile heap is mapped to both logical tiles with
  `REUSE_SINGLE_TILE`; a subsequent `SKIP` update leaves the alias intact, and
  a copy/readback observes the exact source bytes. The same run exercises the
  documented single-region/default-region and omitted range-count forms.
  Mapping is issued on one direct queue, synchronized with a fence wait on a
  second direct queue, and the second queue performs the verified readback.
- Placement-texture `CopyTileMappings` is behavior-backed across array slices:
  a two-tile source mapping is copied across both slices of an independently
  created two-slice reserved texture, then a queued copy/readback observes both exact
  source tiles after an aliasing barrier. The two-tile linear range crosses the
  array-slice boundary; the implementation expands such ranges across slices
  while retaining volume Z addressing. The array mapping uses the same direct
  queue/fence ordering and second-queue readback path.
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
  "device.adapter_luid_verified": true,
  "buffers.default_cpu_io_verified": true,
  "buffers.residency_state_verified": true,
  "buffers.residency_refcount_first_evict_map": "0x00000000",
  "buffers.residency_refcount_second_evict_map": "0x887a0001",
  "shared_handles.roundtrip_verified": true,
  "shared_handles.independent_objects_verified": true,
  "shared_handles.cross_process_verified": true,
  "shared_handles.heap_roundtrip_verified": true,
  "shared_handles.heap_cross_process_verified": true,
  "shared_handles.fence_cross_process_verified": true,
  "shared_handles.invalid_create_access": "0x80070057",
  "shared_handles.invalid_open_access": "0x80070057",
  "buffers.address_heap_open_verified": true,
  "buffers.heap_aliasing_verified": true,
  "buffers.heap_residency_verified": true,
  "buffers.texture_gpu_va_zero": true,
  "resource_shapes.all_created_and_roundtripped": true,
  "resource_shapes.validation_matrix_verified": true,
  "resource_shapes.footprint_matrix_verified": true,
  "resource_shapes.invalid_zero_width": "0x80070057",
  "resource_shapes.invalid_committed_heap_flags": "0x80070057",
  "resource_shapes.invalid_heap_properties": "0x80070057",
  "resource_shapes.invalid_node_mask": "0x80070057",
  "resource_shapes.invalid_upload_state": "0x80070057",
  "resource_shapes.invalid_readback_state": "0x80070057",
  "resource_shapes.invalid_buffer_resource_flags": "0x80070057",
  "resource_shapes.invalid_texture_resource_flags": "0x80070057",
  "resource_shapes.invalid_depth_format_flags": "0x80070057",
  "resource_shapes.invalid_clear_without_flag": "0x80070057",
  "resource_shapes.invalid_clear_format": "0x80070057",
  "resource_shapes.null_heap_properties": "0x80070057",
  "resource_shapes.invalid_zero_width_allocation": [0, 0],
  "resource_shapes.invalid_msaa_mips": "0x80070057",
  "resource_shapes.invalid_msaa_mips_allocation": [0, 0],
  "resource_shapes.volume_allocation": [65536, 65536],
  "resource_shapes.null_allocation": [0, 0],
  "resource_shapes.null_sideband": [0, 0, 0],
  "resource_shapes.invalid_footprint_total": 18446744073709551615,
  "resource_shapes.planar_footprint_total": 12032,
  "resource_shapes.planar_footprint_verified": true,
  "resource_shapes.nv12_footprint_total": 3072,
  "resource_shapes.nv12_footprint_verified": true,
  "resource_shapes.invalid_nv12_height": "0x80070057",
  "resource_shapes.tight_alignment": {
    "feature_tier": 1,
    "allocation": [1024, 256],
    "placed_roundtrip_verified": true,
    "invalid_alignment": "0x80070057",
    "overaligned_placed": "0x80070057",
    "reserved_rejected": "0x80070057"
  },
  "resource_shapes.full_mip_count": 6,
  "resource_shapes.full_mip_footprint_verified": true,
  "resource_shapes.not_resident_roundtrip_verified": true,
  "resource_shapes.enqueue_make_resident": ["0x00000000", 9],
  "resource_shapes.invalid_enqueue_flags": "0x80070057",
  "resource_shapes.descriptor_heap_residency_verified": true,
  "resource_shapes.query_heap_residency_verified": true,
  "resource_shapes.misaligned_placement": "0x80070057",
  "resource_shapes.invalid_heap_alignment": "0x80070057",
  "resource_shapes.invalid_heap_flags": "0x80070057",
  "textures.direct_io_texture_verified": true,
  "textures.direct_io_volume_verified": true,
  "textures.unaligned_bc1_direct_io_verified": true,
  "textures.unaligned_bc1_copy_verified": true,
  "formats.D24_UNORM_S8_UINT.plane_count": 2,
  "castable_formats.relaxed_castable_formats": {
    "pass": true,
    "observed_bits": 1065353216,
    "placed_observed_bits": 1065353216,
    "alias_observed_bits": 1073741824,
    "rgba8_uint_values": [0, 0, 128, 63],
    "undeclared_view_rejected": true,
    "invalid_castable_list_rejected": true
  },
  "sparse.unmapped_zero_verified": true,
  "sparse.tier3_physical_page_ownership_verified": true,
  "sparse.volume_copy_verified": true,
  "sparse.volume_alias_copy_verified": true,
  "sparse.reserved_buffer_reuse_single_tile_skip_verified": true,
  "sparse.cross_queue_mapping_wait_verified": true,
  "sparse.array_alias_mapping_verified": true,
  "sparse.array_alias_first_byte": 7,
  "sparse.array_alias_second_byte": 11,
  "sparse.packed_tail_reserved": {
    "total_tiles": 22,
    "packed_mips": [3, 1, 1, 21],
    "tilings": [[4, 4, 1, 0], [2, 2, 1, 16], [1, 1, 1, 20], [0, 0, 0, 4294967295]],
    "partial_query_count": 2,
    "partial_query_verified": true
  }
}
```

The same result records exact successful creation, queue execution, signal,
wait, map, copy, readback, residency, and invalid-handle HRESULTs. The child
process is bounded to 30 seconds and the source wrapper removes its disposable
Wine clone and prefix on every exit path.

### Relaxed castable-format and heap restriction proof

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --resource-views-formats-only
```

The focused view probe passed the castable case with `observed_bits=0x3f800000`,
`placed_observed_bits=0x3f800000`, `alias_observed_bits=0x40000000`, and
`rgba8_uint_values=[0,0,128,63]`. It also returned exact `E_INVALIDARG` for the
invalid castable list and left the undeclared `R32_SINT` view unusable. The same
run exercises the corrected composite heap restriction mask and reports
`cube_six_slice_array=true` with `cube_face_io_verified=true`, and
`texture1d_array_face_io_verified=true`. Its D24 plane lane reports
`depth_plane_bytes_verified=true`, `stencil_plane_bytes_verified=true`, and
`stencil_clear_verified=true`, with all reported plane/clear HRESULTs equal
`0x00000000`.

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
