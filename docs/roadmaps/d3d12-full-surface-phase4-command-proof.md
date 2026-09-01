# Full-Surface Phase 4 Command Recording and Replay Proof

**Status:** Phase 4 command milestone **[COMPLETE]**
**Stable runtime:** MetalSharp Wine 11.5 / Apple M4 / Metal 4
**Stable Agility lane:** 1.619.5 (`D3D12SDKVersion=619`)

## Implemented behavior

- `AtomicCopyBufferUINT` and `AtomicCopyBufferUINT64` record bounded buffer-copy
  commands and retain destination, source, and dependent resources. The
  resource probe checks independent 32-bit and 64-bit readbacks.
- `DiscardResource` records the requested resource and subresource range and
  zeroes buffer backing during replay, including CPU-visible and GPU-private
  buffers.
- Predication is retained in command-list state and gates draw, dispatch,
  indirect, buffer-copy, texture-copy, resource-copy, and resolve replay.
- ExecuteIndirect replays every declared argument kind used by the command
  signature probe, including GPU-only argument/count buffers through the
  validated resource snapshot path and root constants through a staged constant
  buffer. GPU-only argument snapshots honor nonzero argument-buffer offsets and
  are sized after count-buffer clamping; multiplication, offset, stride, and
  64-MiB snapshot bounds are fail-closed. The command probe now also records
  direct and indirect DISPATCH_RAYS and DISPATCH_MESH commands, then verifies
  exact UAV/raster readbacks for both paths.
- Enhanced barrier groups serialize their global, buffer, and texture records
  rather than only their counts. Queue replay applies enhanced buffer access
  generations and texture layout transitions for the recorded subresource
  ranges before the queue-order event split.
- A bounded stream-output provider compiles non-rasterized DXBC vertex-only
  PSOs through the existing SM50 stream-output lowering, binds one output
  buffer at the provider slot, orders a filled-size counter update through a
  blit copy, and rejects unsupported stage mixes, DXIL, indexed/multi-instance,
  multiple-stream, and out-of-range draws instead of writing out of bounds.
- Command-stream statistics retain a histogram and unknown-type count for every
  serialized command kind. Render-pass begin/end, protected-session, and
  meta-command calls are serialized and replayed as explicit provider-boundary
  records; unavailable protected/meta providers are logged as not executed.
- `SetSamplePositions` records validated 1/2/4/8/16/32-sample patterns for
  one-, two-, and four-pixel blocks (up to 128 fixed-point positions). Direct
  draw replay expands a multi-pixel block into scissored render passes and
  applies the corresponding sample subset to each Metal MSAA pass; reset and
  malformed pattern records are fail-closed.

## Exact evidence

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --command-replay-only
```

The isolated source-staged command probe passed with these behavior checks, including exact GPU-only indirect DRAW, view-instance array-layer, and multi-pixel MSAA readbacks:

```json
{
  "pass": true,
  "execute_indirect_root_constants": {
    "pass": true,
    "args_write": "0x00000000",
    "count_write": "0x00000000",
    "dispatch_root_constants_verified": true,
    "values": [31, 32, 33, 34],
    "argument_offset": 32,
    "max_command_count": 2,
    "count_clamped_to": 1,
    "offset_and_count_bounds_verified": true
  },
  "command_signature_validation": {
    "pass": true,
    "argument_kind_matrix_created": 7,
    "argument_kind_matrix_verified": true,
    "invalid_stride_limit": "0x80070057",
    "invalid_root_index": "0x80070057",
    "invalid_root_type": "0x80070057",
    "invalid_slot": "0x80070057"
  },
  "execute_indirect_graphics_readback": {
    "pass": true,
    "argument_kind": "DRAW",
    "argument_buffer_gpu_only": true,
    "pixel": [255, 0, 0, 255],
    "pixel_verified": true
  },
  "execute_indirect_indexed_graphics_readback": {
    "pass": true,
    "argument_kinds": ["VBV", "IBV", "DRAW_INDEXED"],
    "argument_buffer_gpu_only": true,
    "pixel": [255, 0, 0, 255],
    "pixel_verified": true
  },
  "execute_indirect_root_descriptors": {
    "pass": true,
    "argument_kinds": ["CBV", "SRV", "UAV", "DISPATCH"],
    "argument_buffer_gpu_only": true,
    "values": [31, 32, 33, 34],
    "readback_verified": true
  },
  "execute_indirect_dispatch_rays": {
    "pass": true,
    "state_object_created": true,
    "direct_dispatch_recorded": true,
    "indirect_dispatch_recorded": true,
    "direct_value": "0x52415931",
    "indirect_value": "0x52415931",
    "direct_behavior_verified": true,
    "indirect_behavior_verified": true,
    "argument_offset": 16
  },
  "execute_indirect_dispatch_mesh": {
    "pass": true,
    "pso_created": true,
    "direct_dispatch_recorded": true,
    "indirect_dispatch_recorded": true,
    "direct_uav_value": "0x4d455348",
    "indirect_uav_value": "0x4d455348",
    "direct_pixels": 72,
    "indirect_pixels": 72,
    "direct_behavior_verified": true,
    "indirect_behavior_verified": true,
    "argument_offset": 16
  },
  "predication": {
    "pass": true,
    "feature_supported": true,
    "executed_value": 1,
    "suppressed_dispatch_value": 0,
    "suppressed_copy_value": 0
  },
  "enhanced_barriers": {
    "pass": true,
    "copy_values_verified": true,
    "global_buffer_texture_records_replayed": true
  },
  "view_instancing_mask_side_effect": {
    "pass": true,
    "pipeline_view_instance_count": 2,
    "mask_first": 1,
    "mask_second": 2,
    "slice0_rgba": [255, 0, 0, 255],
    "slice1_rgba": [0, 255, 0, 255]
  },
  "multi_pixel_sample_positions": {
    "pass": true,
    "sample_count": 4,
    "pixel_count": 4,
    "pixel0_black": true,
    "pixel1_red": true,
    "pixel2_black": true,
    "pixel3_black": true,
    "pixels_rgba": [[0, 0, 0, 255], [255, 0, 0, 255], [0, 0, 0, 255], [0, 0, 0, 255]]
  },
  "queue_validation": {
    "pass": true,
    "normal_high_queue_types_verified": true,
    "high_priority_ok": true,
    "vblank_verified": true,
    "invalid_bundle": "0x80070057",
    "video_queue": "0x80004001",
    "global_realtime_queue": "0x80004001",
    "disable_gpu_timeout": "0x80070057",
    "clock_calibration_verified": true,
    "enqueue_event_signaled_after_all_queues": true
  },
  "stream_output_capture": {
    "pass": true,
    "provider": "sm50_vertex_capture",
    "vertex_count": 4,
    "stride": 16,
    "filled_size": 144,
    "initial_filled_size": 16,
    "filled_size_offset": 4,
    "draw_count": 3,
    "captured_draw_count": 2,
    "overflow_guard_verified": true,
    "initial_region_untouched": true,
    "output_verified": true,
    "filled_size_verified": true
  },
  "command_inventory": {
    "histogram": true,
    "unknown_type_count": 1,
    "unit_contract_test": "vendor/dxmt/tests/dxil/test_d3d12_vertex_contract.cpp"
  }
}
```

The resource probe independently records `atomic_copy_verified=true` and
`discard_verified=true`. The queue probe also passed with nonzero GPU/CPU clock
calibration values, a 1 GHz timestamp frequency, high-priority queue
round-trip validation, CoreVideo display-link VBlank completion, cross-queue
fence ordering, and exact null-output `E_POINTER` validation. The Unix
Winemetal bridge now exposes the display-link wait with the same normal and
WOW64 call-table index. With `DXMT_WINEMETAL_DEBUG=1`,
the Unix provider log recorded `encoder_insert_debug_signpost`,
`encoder_push_debug_group`, and `encoder_pop_debug_group` calls for the
recorded marker/event commands. The D3D10/D3D11 legacy
regression probe also passed against the same rebuilt runtime. The updated
Winemetal bridge ABI check passed with the expanded render-pass structure and
three debug-annotation exports, with no missing PE, Unix, or WOW64 entries.
All child waits
are bounded and the source wrapper removes its disposable Wine clone and prefix
after each invocation.

The fail-closed coverage manifest is
`tools/d3d12-metal-sdk/contracts/phase4-command-coverage.json`. The latest
source-staged command probe closes the complete indirect-argument replay row
with exact direct/indirect DISPATCH_RAYS and DISPATCH_MESH readbacks. View-
instancing, multi-pixel sample-position, and queue-priority/VBlank/callback
rows also have exact provider evidence; broader indirect and feature-family
matrices remain tracked by the later phase lanes.

## Residual limitations carried into later phases

The stream-output provider is limited to one non-rasterized DXBC vertex stream
and still lacks multiple streams, overflow continuation, DXIL/geometry-stage
capture, and downstream-consumer coverage. The source-staged counter lane now
starts at a nonzero filled size (`16` bytes), preserves that prefix, appends two
exact draws, and reads back the final `144`-byte counter. Broader
sample-count/pattern and indirect-work coverage remains in the later phase
lanes. Protected-session/meta-command providers, markers/events with
independent readback, nested ExecuteIndirect behavior, and broader command
inventory side-effect matrices remain explicitly ledgered; none are silently
promoted by the completed Phase 4 contract. The sample-position provider is currently
proven for direct draws with a four-sample 2x2 pattern; broader sample-count/
pattern and indirect-work coverage belongs to the Phase 6/indirect-work lanes.
