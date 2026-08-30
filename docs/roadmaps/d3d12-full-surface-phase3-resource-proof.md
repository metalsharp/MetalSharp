# Full-Surface Phase 3 Resource, Residency, and Sharing Proof

**Status:** Phase 3 complete on the stable isolated proof host
**Stable runtime:** MetalSharp Wine 11.5 / Apple M4 / Metal 4
**Stable Agility lane:** 1.619.5 (`D3D12SDKVersion=619`)

## Implemented behavior

- CPU-visible upload/readback and GPU-upload buffers, plus default-resource
  `ReadFromSubresource`/`WriteToSubresource`, now use validated buffer copies;
  committed and placed GPU-upload buffers plus committed and placed RGBA8
  GPU-upload textures map/round-trip exact bytes. The committed GPU-upload
  texture matrix also round-trips R8_UNORM, R16_FLOAT, and
  R16G16B16A16_UNORM. An equivalent custom
  write-back/L0 heap also places and round-trips an RGBA8 texture. Options16
  reports `GPUUploadHeapSupported`, `GetCustomHeapProperties` returns the
  cache-coherent UMA mapping, and shared/shared-cross-adapter GPU-upload flags
  reject. An
  out-of-bounds `Map` read range rejects with `E_INVALIDARG` and leaves the
  output pointer null.
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
  process can reconstruct buffers, a heap, a texture, and a fence; observe
  shared writes/signals; and return the child result through the same backing.
  Queue `Signal` propagates completion into the pointer-free fence mapping, and
  an independently reopened fence bridges that mapping into a queue `Wait`.
  The probe also covers read-only buffer/heap/texture/fence rights, stronger
  access rejection, adapter-LUID checks, and disappearance of a named mapping
  after its final owner and handle are released.
- `OpenExistingHeapFromAddress` resolves a live mapped heap only for the owning
  device, and `OpenExistingHeapFromFileMapping` validates and reconstructs a
  shared CPU-visible heap.
- Resource lifetime, placed-resource aliasing, reserved buffers/textures,
  packed and partial mips, volume tiling, format variants, tile mapping,
  unmapped zeroing, and physical-page ownership are behavior-backed by the
  resource probe. The advertised sparse format matrix now executes exact
  one-tile round-trips for **66** stable-provider formats, including every
  R8/R8G8/R16/RGBA8/R10/R11/R32/R32G32/RGBA16/RGBA32 and BC variant whose
  `FORMAT_SUPPORT2_TILED` result is promoted. Non-proven sparse formats are
  not advertised as tiled.
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
  exact row and slice pitches and byte readback; a nonzero-Z `2x2x1` boxed
  volume write/read also round-trips exact bytes. Additional legal packed
  format variants (B8G8R8X8, B5G6R5, B5G5R5A1, B4G4R4A4, R9G9B9E5, and the
  4:2:2 packed forms) now create through their matching Metal pixel formats
  and pass format-info/support queries. The D3D12 format-support matrix also
  covers the typeless depth/stencil and 10-bit cases without advertising a
  provider-less texture format; a provider-less R32G32B32 format query
  returns exact `E_INVALIDARG`. Direct default-resource I/O also
  round-trips exact packed bytes for B5G6R5, B5G5R5A1, B4G4R4A4, and
  R9G9B9E5. Unsupported `R1_UNORM` creation and allocation-info queries both
  fail closed without returning a fabricated allocation.
- A two-slice 1D-array resource and a six-face cube resource (the D3D12
  six-slice 2D representation) create successfully; D16, R10G10B10A2
  typeless, and the R24/R32 depth-stencil typeless/plane format family also
  create with matching Metal depth/swizzle providers. Logical D3D12 1D and
  1D-array resources use height-one Metal 2D/2D-array backing because native
  Metal 1D arrays cannot carry mip chains. Mipped 1D arrays, mipped 2D arrays,
  mipped 3D volumes, and four-sample 2D MSAA arrays preserve their requested
  mip, extent, slice, and sample shape. The backend preserves the 1D array length, and each 1D-array slice and cube face independently
  round-trips distinct bytes through `WriteToSubresource`/`ReadFromSubresource`.
  A placed two-slice 1D array also uses the correct Metal array length and
  independently round-trips both slices through a default placement heap. A
  placed RGBA8 2D texture at a nonzero 64-KiB heap offset also round-trips
  exact bytes.
- NV12 footprints expand luma/chroma into R8/R8G8 planes with half-resolution
  chroma dimensions (`13x8` plus `7x4`, 3072 bytes total); odd NV12 heights
  reject with `E_INVALIDARG`. Committed NV12 and P010 resources use an explicit
  tightly packed CPU multi-plane provider: luma and chroma `WriteToSubresource`
  and `ReadFromSubresource` calls round-trip exact bytes on both planes rather
  than treating a single-plane Metal texture as planar data. The command
  replay path also copies an R8 luma footprint from an upload buffer into the
  NV12 resource and back to a readback buffer, with exact row-pitch-preserving
  bytes. `CopyResource` also copies both NV12 planes into an independent
  resource and the destination is read back byte-for-byte.
- Planar depth/stencil footprints expand the D24/R24G8 family into the
  documented R32 depth and R8 stencil planes across array slices and mips;
  eight subresources report exact plane formats, row sizes, dimensions, and
  ordered offsets (`12032` total bytes). A D32_FLOAT_S8X24_UINT footprint
  reports R32/R8 planes, 256-byte row pitches, and the exact 2048-byte total.
  D24 and D32_FLOAT_S8X24_UINT plane-0 and plane-1 direct I/O round-trip exact
  depth/stencil bytes, and a queued
  stencil-only clear is visible through a subsequent plane-1 readback.
- Invalid `GetCopyableFootprints` descriptors initialize the documented
  `pTotalBytes` sentinel (`UINT64_MAX`) instead of reporting a fabricated
  zero-sized layout. Checked arithmetic also returns that sentinel for a valid
  footprint whose base offset would overflow. Valid requests honor the
  caller's requested subresource window: a one-subresource packed-mip query
  writes only that footprint and leaves adjacent guard entries untouched.
- Allocation-info and sideband allocation-info calls with a nonzero count and
  null descriptor arrays initialize `[SizeInBytes=0, Alignment=0]` and zero
  sideband output without dereferencing the caller's null pointer. Checked
  allocation arithmetic rejects a `UINT64_MAX` buffer size with the same zero
  allocation result instead of wrapping. Unsupported hardware-protected,
  write-watch, shader-atomic, manual-write-tracking, create-not-zeroed, and
  cross-adapter heap flags reject with exact `E_INVALIDARG` rather than
  creating an unbacked heap. A two-resource sideband query also
  reports the aggregate 128-KiB size and aligned offsets `0`/`65536`. A
  mixed 64-KiB/4-MiB/64-KiB batch reports the required 12-MiB aggregate,
  4-MiB alignment, and middle-resource placement at `4 MiB`.
- Committed-resource validation rejects a null heap-properties pointer, a
  buffer placed in a heap with `DENY_BUFFERS`, the invalid simultaneous-access
  and unsupported cross-adapter buffer flags, simultaneous-access MSAA,
  unsupported video/reference and raytracing resource flags, the unsupported
  standard-swizzle layout, 1D/2D/3D dimensions above the D3D12
  request limits, and a
  reserved texture using `UNKNOWN` layout;
  all return exact `E_INVALIDARG` before allocating any object. Valid reserved
  textures use the required `64KB_UNDEFINED_SWIZZLE` layout.
- `ID3D12Device::GetAdapterLuid` returns a stable nonzero adapter identity in
  the same source-staged run as the resource/share tests; the existing DXGI
  factory lane cross-checks that identity against adapter enumeration.
- Named mapping handles no longer retain process-global COM references; each
  `OpenSharedHandleByName` call opens and validates a fresh file-mapping handle;
  buffer, heap, and fence metadata carries the adapter LUID and reopening
  rejects a mapping whose identity does not match the current device, while
  `OpenSharedHandle` reconstructs named objects first and uses the legacy
  registry only for unsupported object kinds. Unnamed buffer handles also use a
  generated, pointer-free file-mapping transport and independently reopen with
  exact byte readback in the creating process. An inheritable unnamed resource
  mapping is passed as a real `HANDLE` to an independent Wine process; the
  child opens the buffer and fence, verifies the initial bytes and signaled
  fence value, writes a sentinel, and the parent observes that write after the
  child exits. An inherited unnamed heap handle is independently opened by a
  second child and its heap metadata is validated without a process-local
  registry entry. Mutating a mapping's stored adapter LUID makes
  `OpenSharedHandle` reject it with exact `DXGI_ERROR_INVALID_CALL`
  (`0x887A0001`), then the original metadata is restored before the valid
  reopen.
- `OpenSharedHandle` reconstructs named mapping-backed resources as
  independent COM objects (rather than returning a process-global registry
  pointer); descriptor identity and shared bytes remain valid across the
  original and both opened objects.
- Shared-handle creation and named opening reject zero access masks with
  exact `E_INVALIDARG`; unsupported shared-cross-adapter fence creation also
  rejects with exact `E_INVALIDARG`. Valid named buffer, heap, fence, and
  committed shared RGBA8 texture mappings retain their cross-process behavior.
  The shared texture uses a Metal `MTLSharedTextureHandle` Mach-port service;
  an ordinary non-shared texture still returns the explicit `E_NOTIMPL`
  provider boundary rather than a process-local success.
  A non-CPU-visible default heap is rejected the same way, while CPU-visible
  upload heaps remain shareable through the file-mapping provider.
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
  marker) rather than claiming the full resource count. A boundary query with
  `FirstSubresourceTilingToGet=UINT_MAX` returns zero and leaves its output
  guard untouched.
- Reserved 512x512 RGBA8 resources with four mips now create successfully and
  report the packed tail exactly: 22 total tiles, three standard mips, one
  packed mip tile, and per-subresource starts `[0, 16, 20, D3D12_PACKED_TILE]`.
  A two-slice version reports 44 total tiles and starts the second slice at
  tiles `[22, 38, 42, D3D12_PACKED_TILE]`; both packed-tail slice mips are
  mapped, copied, and read back exactly. The packed mip is also mapped and
  transferred through standard
  `CopyTextureRegion` calls (the API required for packed mips), with a 64x64
  RGBA8 footprint (`row_size=256`, `total_bytes=16384`) and exact readback.
- A native reserved two-slice 1D `R32_FLOAT` resource (`16384` texels per
  slice, one mip) now creates successfully and reports the exact tile shape
  `[16384,1,1]`, two subresource tilings starting at tiles 0 and 1, and two
  logical tiles. A second native reserved 1D array receives the same two-tile
  mapping through `CopyTileMappings`; the source-staged run maps eight Metal
  16-KiB pages, executes `CopyTiles` across both slices in both directions,
  and verifies the exact 128-KiB readback without falling back to a failed
  sparse heap allocation.
- Zero `MipLevels` is normalized to the complete mip chain across dimensions:
  1D-array `17x1` creates five mips, a 2D-array `19x11` creates five mips, a
  3D `7x5x4` volume creates three mips, and the existing 32x16 texture creates
  six mips. The six 2D footprints report exact dimensions, row counts, row
  sizes, and increasing placement offsets.
- Small-resource placement alignment is behavior-backed: an 8x8 RGBA8 texture
  reports `[SizeInBytes=4096, Alignment=4096]`, and an 8x8 four-sample MSAA
  texture reports `[SizeInBytes=65536, Alignment=65536]`; a large texture,
  oversized MSAA texture, or non-MSAA texture requesting MSAA alignment rejects
  with `E_INVALIDARG`. Stable tight alignment Tier 1 is behavior-backed for
  buffers: a committed 1000-byte buffer reports
  `[SizeInBytes=1024, Alignment=256]`, a 256-byte
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
- D3D12 resources expose the DXGI resource aggregate used by residency APIs.
  A source-staged `OfferResources` → `IDXGIDevice3::Trim` →
  `ReclaimResources` cycle reports the resource evicted after trim, sets the
  discarded bit, and reports it fully resident after reclaim; priority values
  preserve the DXGI/D3D12 high-priority encoding difference.
- Unnamed CPU-visible heap and fence handles use generated, pointer-free file
  mappings rather than the legacy process-local registry. Independent objects
  reopen from each handle; the unnamed fence observes an asynchronous queue
  signal at value `13`.
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
  "buffers.invalid_map_range_verified": true,
  "buffers.dxgi_offer_reclaim_verified": true,
  "buffers.offered_resource_discarded": true,
  "buffers.offered_resource_priority": 2684354560,
  "buffers.residency_refcount_first_evict_map": "0x00000000",
  "buffers.residency_refcount_second_evict_map": "0x887a0001",
  "shared_handles.roundtrip_verified": true,
  "shared_handles.independent_objects_verified": true,
  "shared_handles.unnamed_roundtrip_verified": true,
  "shared_handles.unnamed_cross_process_verified": true,
  "shared_handles.unnamed_fence_cross_process_verified": true,
  "shared_handles.cross_process_verified": true,
  "shared_handles.unnamed_heap_cross_process_verified": true,
  "shared_handles.adapter_luid_mismatch": "0x887a0001",
  "shared_handles.adapter_luid_metadata_verified": true,
  "shared_handles.heap_roundtrip_verified": true,
  "shared_handles.heap_cross_process_verified": true,
  "shared_handles.unnamed_heap_roundtrip_verified": true,
  "shared_handles.default_heap_handle_create": "0x80004001",
  "shared_handles.unnamed_fence_roundtrip_verified": true,
  "shared_handles.fence_cross_process_verified": true,
  "shared_handles.fence_mapping_signal": "0x00000000",
  "shared_handles.fence_wait_mapping": "0x00000000",
  "shared_handles.invalid_create_access": "0x80070057",
  "shared_handles.invalid_open_access": "0x80070057",
  "shared_handles.invalid_shared_cross_adapter_fence": "0x80070057",
  "buffers.address_heap_open_verified": true,
  "buffers.heap_aliasing_verified": true,
  "buffers.heap_residency_verified": true,
  "buffers.texture_gpu_va_zero": true,
  "resource_shapes.all_created_and_roundtripped": true,
  "resource_shapes.validation_matrix_verified": true,
  "resource_shapes.footprint_matrix_verified": true,
  "resource_shapes.zero_mip_counts": [5, 5, 3],
  "resource_shapes.zero_mip_shapes_verified": true,
  "resource_shapes.placed_1d_array_io_verified": true,
  "resource_shapes.placed_2d_io_verified": true,
  "resource_shapes.gpu_upload_resource_io_verified": true,
  "resource_shapes.gpu_upload_texture_io_verified": true,
  "resource_shapes.gpu_upload_placed_io_verified": true,
  "resource_shapes.gpu_upload_texture_placed_io_verified": true,
  "resource_shapes.custom_gpu_upload_texture_placed_io_verified": true,
  "resource_shapes.gpu_upload_feature_verified": true,
  "resource_shapes.gpu_upload_custom_properties_verified": true,
  "resource_shapes.gpu_upload_texture_format_matrix_count": 3,
  "resource_shapes.gpu_upload_texture_format_matrix_verified": true,
  "resource_shapes.gpu_upload_invalid_shared": "0x80070057",
  "resource_shapes.gpu_upload_heap_invalid_shared": "0x80070057",
  "resource_shapes.invalid_zero_width": "0x80070057",
  "resource_shapes.oversized_1d": "0x80070057",
  "resource_shapes.oversized_2d": "0x80070057",
  "resource_shapes.oversized_3d": "0x80070057",
  "resource_shapes.invalid_reserved_layout": "0x80070057",
  "resource_shapes.invalid_standard_swizzle": "0x80070057",
  "resource_shapes.invalid_small_texture_alignment": "0x80070057",
  "resource_shapes.invalid_msaa_small_alignment": "0x80070057",
  "resource_shapes.invalid_nonmsaa_msaa_alignment": "0x80070057",
  "resource_shapes.small_texture_allocation": [4096, 4096],
  "resource_shapes.small_msaa_allocation": [65536, 65536],
  "resource_shapes.invalid_committed_heap_flags": "0x80070057",
  "resource_shapes.invalid_heap_properties": "0x80070057",
  "resource_shapes.invalid_node_mask": "0x80070057",
  "resource_shapes.invalid_upload_state": "0x80070057",
  "resource_shapes.invalid_readback_state": "0x80070057",
  "resource_shapes.invalid_buffer_resource_flags": "0x80070057",
  "resource_shapes.invalid_buffer_simultaneous": "0x80070057",
  "resource_shapes.invalid_cross_adapter": "0x80070057",
  "resource_shapes.invalid_simultaneous_msaa": "0x80070057",
  "resource_shapes.invalid_video_resource_flags": "0x80070057",
  "resource_shapes.invalid_raytracing_resource_flags": "0x80070057",
  "resource_shapes.invalid_texture_resource_flags": "0x80070057",
  "resource_shapes.invalid_depth_format_flags": "0x80070057",
  "resource_shapes.invalid_clear_without_flag": "0x80070057",
  "resource_shapes.invalid_clear_format": "0x80070057",
  "resource_shapes.null_heap_properties": "0x80070057",
  "resource_shapes.invalid_zero_width_allocation": [0, 0],
  "resource_shapes.allocation_overflow": [0, 0],
  "resource_shapes.allocation_batch": [131072, 65536, 0, 65536, 65536],
  "resource_shapes.allocation_batch_verified": true,
  "resource_shapes.allocation_mixed": [12582912, 4194304, 0, 4194304, 8388608],
  "resource_shapes.allocation_mixed_verified": true,
  "resource_shapes.invalid_msaa_mips": "0x80070057",
  "resource_shapes.invalid_msaa_mips_allocation": [0, 0],
  "resource_shapes.volume_allocation": [65536, 65536],
  "resource_shapes.null_allocation": [0, 0],
  "resource_shapes.null_sideband": [0, 0, 0],
  "resource_shapes.invalid_footprint_total": 18446744073709551615,
  "resource_shapes.footprint_overflow_total": 18446744073709551615,
  "resource_shapes.planar_footprint_total": 12032,
  "resource_shapes.planar_footprint_verified": true,
  "resource_shapes.d32_footprint_total": 2048,
  "resource_shapes.d32_footprint_verified": true,
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
  "resource_shapes.unsupported_heap_flags": "0x80070057",
  "textures.direct_io_texture_verified": true,
  "textures.shared_texture_create": "0x80004001",
  "textures.shared_texture_resource_create": "0x00000000",
  "textures.shared_texture_handle_create": "0x00000000",
  "textures.shared_texture_open": "0x00000000",
  "textures.shared_texture_named_open": "0x00000000",
  "textures.shared_texture_roundtrip_verified": true,
  "textures.shared_texture_cross_process_verified": true,
  "textures.unsupported_r1_allocation": [0, 0],
  "textures.unsupported_rgb32_allocation": [0, 0],
  "textures.direct_io_volume_verified": true,
  "textures.direct_io_volume_box_verified": true,
  "textures.direct_io_d32s8_verified": true,
  "textures.direct_io_nv12_verified": true,
  "textures.direct_io_p010_verified": true,
  "textures.direct_io_variant_verified": true,
  "textures.direct_io_nv12_copy_verified": true,
  "textures.direct_io_nv12_resource_copy_verified": true,
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
  "sparse.reserved_1d_resource_create": "0x00000000",
  "sparse.reserved_1d_tiling": "0x00000000",
  "sparse.reserved_1d_total_tiles": 2,
  "sparse.reserved_1d_tiling_count": 2,
  "sparse.reserved_1d_tile_shape": [16384, 1, 1],
  "sparse.reserved_1d_tilings": [[1,0],[1,1]],
  "sparse.reserved_1d_copy_verified": true,
  "sparse.reserved_1d_mapping_copy_verified": true,
  "sparse.tier3_physical_page_ownership_verified": true,
  "sparse.volume_copy_verified": true,
  "sparse.volume_alias_copy_verified": true,
  "sparse.reserved_buffer_reuse_single_tile_skip_verified": true,
  "sparse.cross_queue_mapping_wait_verified": true,
  "sparse.array_alias_mapping_verified": true,
  "sparse.array_alias_first_byte": 7,
  "sparse.array_alias_second_byte": 11,
  "sparse.format_matrix_count": 66,
  "sparse.format_matrix_all_tile_shapes_verified": true,
  "sparse.dimension_matrix_verified": true,
  "sparse.packed_tail_reserved": {
    "array_total_tiles": 44,
    "array_tiling_count": 8,
    "array_query_verified": true,
    "array_copy_verified": true,
    "total_tiles": 22,
    "footprint_verified": true,
    "copy_verified": true,
    "boundary_query_verified": true,
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

The fail-closed aggregate gate is now executable as well:

```sh
python3 tools/d3d12-metal-sdk/scripts/validate-full-surface-gate.py \
  --phase 3 --profile metalsharp-isolated
```

The tracked exhaustive-coverage manifest is now closed only after the latest
source-staged result supplied the required positive checks. Running the command
above is the reproducible Phase 3 gate; it passes for `--phase 3` and remains
fail-closed for the later full-surface phases.

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
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --resources-only
```

The resource probe's `IDXGIDevice3::QueryResourceResidency` path observed
`DXGI_RESIDENCY_FULLY_RESIDENT (1)`, `DXGI_RESIDENCY_EVICTED_TO_DISK (3)`,
and `FULLY_RESIDENT (1)` around D3D12 `Evict`/`MakeResident` and
`OfferResources`/`Trim`/`ReclaimResources` cycles. It additionally allocated
and touched an 8-resource, 512-MiB pressure arena before offering, trimming,
querying, and reclaiming all resources with discarded-state readback.

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

The Phase 3 exit gate is closed on the stable 1.619.5 proof host. The final
source-staged `probe-resources-metalsharp-isolated.json` reports a 108-case
format/shape/subresource matrix, three aligned placed-buffer offsets, a
66-format advertised sparse matrix with physical-page and alias evidence,
512-MiB pressure/reclaim evidence, and named/unnamed cross-process sharing for
buffers, heaps, fences, and textures. Read-only access, lifetime, security,
and adapter-LUID checks are explicit. Unsupported provider combinations remain
fail-closed and are not promoted by the phase gate.
