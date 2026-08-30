# Full-Surface Phase 5 Shader Compiler and SM5.x–SM6.9 Proof

**Status:** Phase 5 shader milestone; exhaustive phase gate remains open
**Stable runtime:** MetalSharp Wine 11.5 / Apple M4 / Metal 4
**Stable Agility lane:** 1.619.5 (`D3D12SDKVersion=619`)

## Implemented behavior

- The typed DXIL-to-MSL path now rejects a generated shader when lowering
  reports unsupported intrinsics or opcodes. An unsupported operation cannot
  silently become a placeholder MSL value and a successful PSO.
- The pinned DXC corpus compiles and links the SM5.0 graphics baseline and
  compute targets SM6.0 through SM6.9. SM6.7/6.8 include vector and 64-bit
  arithmetic; the SM6.9 runtime lane uses `float16_t` with
  `-enable-16bit-types`.
- The exact semantic readback probe covers float/int math and bitcasts, raw
  buffer load/store, UAV exchange/add/compare-exchange atomics, 32-bit
  special-float predicates, group-shared atomics and barriers, WaveOps/QuadOps,
  SM6.7 vector/int64 arithmetic, SM6.8 wide arithmetic, SM6.9 float16 to
  integer conversion, and a 4x4 texture's Load, SampleLevel, SampleGrad,
  SampleBias, GatherRed, and GetDimensions forms. Every lane creates a PSO,
  dispatches through the DXMT command path, and matches its expected
  readback.
- The WaveOps probe independently verifies lane index/count, ballot, lane
  reads, any/all, quad operations, reductions, and prefix behavior on the
  32-lane dispatch, with zero mismatches.
- Existing compute PSO evidence covers CBV/SRV/UAV binding, texture sampling,
  32-bit atomics, indirect dispatch bounds, and explicit append/consume-counter
  policy. Append/consume counters remain fail-closed rather than being
  advertised as implemented.
- The shader diagnostic probe proves malformed DXIL is rejected with a
  stage-specific `shader/bitcode_parse` diagnostic and no PSO object, while
  valid DXBC/DXIL caches and D3DCompile/DXC provenance remain observable. The
  generated-report audit also checks every focused report for nonzero
  unsupported-intrinsic/opcode counts and placeholder lowering markers.

## Exact evidence

The focused source-staged commands are:

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --semantic-only

DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --shader-corpus-only
```

The latest isolated semantic result passed with these exact lanes:

```json
{
  "ok": true,
  "special_float": {"expected": [1, 1, 1, 0], "actual": [1, 1, 1, 0]},
  "atomic_uav": {"expected": [7, 0, 5, 8], "actual": [7, 0, 5, 8]},
  "sm67_vector_int64": {"expected": [68, 69, 70, 71], "actual": [68, 69, 70, 71]},
  "sm68_vector_arithmetic": {"expected": [68, 136, 204, 272], "actual": [68, 136, 204, 272]},
  "sm69_integer_float_mix": {"expected": [69, 70, 72, 73], "actual": [69, 70, 72, 73]},
  "texture_sampling_forms": {"expected": [64, 64, 64, 64, 64, 68], "actual": [64, 64, 64, 64, 64, 68]}
}
```

The shader-corpus summary passed `sm50_baseline`, the SM6.0–SM6.6
progression, and the new `sm67_to_sm69_progression`; its `cs_9_9` negative
case was deterministically rejected. The diagnostic result passed with
`bad_compute_pso=0x80004005` and
`bad_compute_rejected_with_diagnostic=true`. The same probe exercises the
ID3D12Device9 shader-cache control boundary: application-managed disable and
enable return `S_OK`, while zero-kind control returns `E_INVALIDARG` and the
reported `cache_control_valid` flag is true. The same run reports nonzero
SM5 cache artifacts (`vs_metallib`, `ps_metallib`, and `cs_metallib`) and a
complete cache set.

The fail-closed coverage manifest is
`tools/d3d12-metal-sdk/contracts/phase5-shader-coverage.json`. It records the
closed semantic, WaveOps, diagnostic, atomic/special-float, binding-baseline,
and focused lowering-report-audit rows while keeping the exhaustive SM5.x–SM6.9
opcode/stage/resource/cache/session row open.

## Remaining Phase 5 work

The complete exit gate is not claimed. The stable corpus still needs positive
and negative behavior evidence for every declared DXIL opcode/intrinsic,
control-flow and aggregate shape, graphics stage, texture dimension and
sampling form, typed/raw/structured/counter resource, cache/compiler-session
path, and all legal SM5.x–SM6.9 operations. Append/consume UAV counters and
other explicitly limited providers remain fail-closed until their exact
readback matrices pass. `D3D12_FEATURE_SHADER_MODEL` therefore remains at the
behavior-backed 6.7 report; compiling an isolated 6.9 lane does not promote a
full 6.9 capability claim.
