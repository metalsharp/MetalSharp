# Full-Surface Phase 2 COM and Lifecycle Proof

**Status:** Phase 2 complete
**Stable runtime:** MetalSharp Wine 11.5 / Apple M4 / Metal 4
**Stable Agility lane:** 1.619.5 (`D3D12SDKVersion=619`)

## Implemented behavior

- `MTLD3D12InfoQueue` now stores application/runtime messages, enforces the
  message-count limit, tracks allowed/denied/discarded counts, serializes
  retrieval messages, applies storage/retrieval allow/deny filters, maintains
  filter stacks, stores break settings, and stores mute state.
- `MTLD3D12Device` owns one shared InfoQueue instance. Repeated device QI calls
  return the same object with correct COM references, and the device releases
  its owner reference during destruction.
- `ComPrivateData` now validates null/size combinations, handles allocation
  failure, copies zero-length data safely, and treats a null interface as
  deletion rather than storing a null interface entry.
- Existing device, queue, allocator, list, fence, heap, descriptor heap,
  resource, query heap, command signature, root signature, pipeline library,
  shader cache, and InfoQueue objects remain covered by the object lifecycle
  probe.

## Exact evidence

### Object and InfoQueue contract

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --object-contracts-only
```

The source-staged probe passed with:

```json
{
  "pass": true,
  "object_count": 13,
  "info_queue_pass": true
}
```

The InfoQueue portion proves repeated QI identity, message storage and exact
counters, message-size query and description readback, storage deny filters,
retrieval allow filters, filter stack push/pop, break category/severity/ID
settings, mute state, invalid-filter rejection, and clear behavior. The object
portion proves private-data source-copy isolation, short-buffer
`DXGI_ERROR_MORE_DATA`, interface private-data ownership, null-interface
removal, debug-name round trips, deletion, and missing-data behavior across
the constructed object set.

### Legacy regression

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --legacy-regression-only
```

The D3D10/D3D11 clear/copy/readback regression passed after the shared private
lifecycle changes.

### Build, ABI, and cleanup

- Clean DXMT source build passed **158/158** targets.
- Disposable source staging passed **18/18** artifacts.
- `check-winemetal-abi.py` passed with zero missing exports and zero failures
  against that disposable staging.
- Contract and probe-matrix validators passed.
- The source wrapper removed its Wine clone and isolated prefix after each
  probe; no `/tmp` or `/private/tmp` `drive_c`, `system.reg`, `user.reg`, or
  `userdef.reg` markers remain.
- Generated build products, probe caches, and temporary stage data were
  removed before delivery.

Phase 2 completes the implemented COM/private-data/InfoQueue lifecycle layer.
Feature-specific interfaces that require later providers remain governed by
their subsequent phases and are not falsely promoted by this proof.
