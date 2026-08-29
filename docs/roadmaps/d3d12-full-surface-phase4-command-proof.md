# Full-Surface Phase 4 Command Recording and Replay Proof

**Status:** Phase 4 command milestone; full phase remains open
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
  buffer.
- Enhanced barrier groups continue to replay with global, buffer, and texture
  ordering.
- `SetSamplePositions` records a validated tier-1, one-pixel sample pattern;
  replay converts D3D12 fixed-point positions to Metal render-pass positions.
  The provider applies the pattern to multisampled render passes.

## Exact evidence

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --command-replay-only
```

The isolated source-staged command probe passed with these behavior checks:

```json
{
  "pass": true,
  "execute_indirect_root_constants": {
    "pass": true,
    "args_write": "0x00000000",
    "count_write": "0x00000000",
    "dispatch_root_constants_verified": true,
    "values": [31, 32, 33, 34]
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
    "copy_values_verified": true
  },
  "programmable_sample_positions": {
    "pass": true,
    "options2_hr": "0x00000000",
    "tier": 1,
    "provider_sample_count": 4,
    "graphics_color_verified": true
  }
}
```

The resource probe independently records `atomic_copy_verified=true` and
`discard_verified=true`. The queue probe also passed with nonzero GPU/CPU clock
calibration values, a 1 GHz timestamp frequency, cross-queue fence ordering,
and exact null-output `E_POINTER` validation. With `DXMT_WINEMETAL_DEBUG=1`,
the Unix provider log recorded `encoder_insert_debug_signpost`,
`encoder_push_debug_group`, and `encoder_pop_debug_group` calls for the
recorded marker/event commands. The D3D10/D3D11 legacy
regression probe also passed against the same rebuilt runtime. The updated
Winemetal bridge ABI check passed with the expanded render-pass structure and
three debug-annotation exports, with no missing PE, Unix, or WOW64 entries.
All child waits
are bounded and the source wrapper removes its disposable Wine clone and prefix
after each invocation.

## Remaining Phase 4 work

The complete exit gate is not claimed. Stream-output capture, tier-2 multi-pixel sample-position patterns,
view-instance masks, protected-session/meta-command paths,
markers/events with real provider side effects, queue priority/VBlank/event
semantics, complete indirect bounds/nested behavior, and exhaustive command
inventory validation remain open and explicitly ledgered.
