# Full-Surface Phase 1 Provider and Synchronization Proof

**Status:** Phase 1 complete
**Stable runtime:** MetalSharp Wine 11.5 / Apple M4 / Metal 4
**Stable Agility lane:** 1.619.5 (`D3D12SDKVersion=619`)

## Implemented architecture

- `vendor/dxmt/src/dxmt/dxmt_capabilities.hpp/.cpp` snapshots the actual Metal
  family, Metal language version, registry ID, working-set values, sample-count
  mask, shared-event support, MTL4 queue support, BC compression, scaler,
  unified-memory, and native-raytracing support at device construction.
- `vendor/dxmt/src/dxmt/dxmt_provider.hpp/.cpp` selects a native Metal,
  compute-emulation, CPU-reference, VideoToolbox, display, shared-resource, or
  protected provider from explicit requirements. Video/display/sharing/
  protected providers remain unavailable until their later implementation
  phases; the selector never silently substitutes a different provider.
- `vendor/dxmt/src/dxmt/dxmt_timeline.hpp` unifies CPU completion and Metal
  shared-event reservation/validation. `CommandQueue` uses it for CPU waits and
  completion and exposes the same underlying shared event to existing D3D11/
  D3D12 paths.
- `vendor/dxmt/src/d3d12/d3d12_resource_state.hpp` tracks global and
  per-subresource legacy state, enhanced layouts, generations, and alias
  invalidation independently of Metal encoder lifetime; queue replay applies
  legacy transitions and records mismatches for provider diagnostics.
- `MTLD3D12Device` logs the host capability snapshot and selected default
  provider without promoting any D3D12 feature query.

## Exact evidence

### Provider policy and timeline

Command:

```sh
tools/d3d12-metal-sdk/scripts/run-provider-architecture-probe.sh
```

Output:

```json
{"schema":"metalsharp.d3d12.provider-probe.v1","pass":true,"case_count":12,"timeline_case":true,"no_silent_fallback":true}
```

The 12 provider cases cover native selection, native-raytracing selection,
compute fallback, disallowed fallback, MTL4 fallback, CPU preference,
no-device fallback, and explicit unavailable video/display/sharing/protected
providers. The timeline case covers monotonic reservation, CPU completion,
completion readback, and rejection of invalid GPU-event encoding handles.

### Real M4 host snapshot

Command:

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --caps-only
```

The source-staged Wine run passed and emitted:

```text
D3D12 host capabilities schema=1 metal=320 registry=0x1000003bd family7=true family8=true family9=true mtl4=true shared_events=true raytracing=true sample_mask=0x16
D3D12 default provider=metal-native available=1 (feature promotion remains behavior-gated)
```

The wrapper removed both the isolated prefix and source Wine clone after the
run.

### Queue synchronization regression

Command:

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --queues-only
```

The source-staged queue probe passed, including command-queue creation,
fences, shared-event completion, timestamp queries, and resource-retention
cleanup. The wrapper removed its isolated prefix and source Wine clone.

The enhanced-barrier/render-pass source probe also passed after the resource
state tracker was integrated into queue replay:

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --barriers-render-pass-only
```

### Build and static contracts

- Clean cross-build passed **158/158** targets after adding the provider and
  capability compilation units.
- `validate-full-surface-contract.py` passed with 145 interfaces, 537 methods,
  and the synchronized 1,028 static inventory findings.
- `validate-contracts.py` passed all 11 contracts.
- `validate-probe-matrix.py` passed all 25 probe groups.
- Shell syntax, Python syntax, clang-format, and `git diff --check` passed.

Phase 1 establishes the provider and synchronization architecture only. The
later feature phases must still prove each provider's operation semantics
before reporting support.
