# Full-Surface Phase 5 Shader Compiler and SM5.x–SM6.9 Proof

**Status:** Phase 5 shader milestone; exhaustive phase gate remains open
**Stable runtime:** MetalSharp Wine 11.5 / Apple M4 / Metal 4
**Stable Agility lane:** 1.619.5 (`D3D12SDKVersion=619`)

## Implemented behavior

- The typed DXIL-to-MSL path now rejects a generated shader when lowering
  reports unsupported intrinsics or opcodes. An unsupported operation cannot
  silently become a placeholder MSL value and a successful PSO.
- The pinned DXC corpus compiles and links the SM5.0 vertex/pixel/geometry
  graphics baseline and compute targets SM6.0 through SM6.9. SM6.7/6.8
  include vector and 64-bit arithmetic; the SM6.9 runtime lane uses `float16_t` with
  `-enable-16bit-types`.
- The exact semantic readback probe covers float/int math and bitcasts,
  first-bit and reverse-bit operations, min/max and integer multiply-add, raw
  buffer load/store, UAV exchange/add/compare-exchange atomics, 32-bit
  special-float predicates, hyperbolic/normal math, `msad4`, SM6.4 `dot2add`
  and signed/unsigned `dot4add` packed accumulation, group-shared atomics and
  barriers,
  WaveOps/QuadOps, SM6.7 vector/int64 arithmetic, SM6.8
  wide arithmetic, SM6.9 float16 to integer conversion, multi-block
  switch/PHI/vector-aggregate and loop-carried aggregate cases, a selected
  double-arithmetic lane, the complete 32-bit atomic binop matrix, and a 4x4
  texture's Load, SampleLevel, SampleGrad, SampleBias, GatherRed, and
  GetDimensions forms, plus signed and unsigned R32 typed texture
  Load/GetDimensions lanes. Every lane
  creates a PSO, dispatches through the DXMT command path, and matches its
  expected readback. The fresh no-offline-converter run reports
  `{1065353216, 0, 0, 1, 7}` for the extended math case.
- The WaveOps probe independently verifies lane index/count, ballot, lane
  reads, any/all, quad operations, reductions, prefix behavior,
  `WaveActiveCountBits`/`WavePrefixCountBits`, `WaveMatch`, and the complete
  `WaveMultiPrefix` sum/product/bitwise/count matrix on the 32-lane dispatch,
  with zero mismatches. The added bit-count case reports 16 active even lanes
  and the expected exclusive prefix count for every lane; the match and
  multi-prefix cases report exact per-value lane masks and group-local
  results. The 64-bit wave sum/product/prefix, signed/unsigned min/max, and
  bitwise cases are lowered through a two-lane `uint` readback decomposition
  because Metal has no 64-bit SIMD reduction, and match every lane exactly.
  The SM6.9 float16 wave cases also compile, link, and read back every lane:
  `wave_f16_sum_prefix` matches the IEEE-754 bits of
  `528 + lane * (lane + 1) / 2`, and `wave_f16_min_max` matches the packed
  minimum/maximum result `0x00200001`.
- The SM6.6/6.7 capability matrix independently verifies root constants,
  descriptor indexing, 32/64-bit atomics, barriers, raw gather, programmable
  and static texture offsets, typed `Texture2D<uint>` load, `SampleCmpLevel`,
  and `QuadVote`; the complete runtime matrix now passes with zero mismatches.
  Its added SM6.8 `SampleCmpGrad` and `SampleCmpBias` compute cases also
  compile, link, dispatch, and read back zero mismatches. The packed-dot cases
  return exact 25 (unsigned) and 23 (signed) values and the exact `dot2add`
  float bits `0x41800000` through the rebuilt typed lowering path.
- Existing compute PSO evidence covers CBV/SRV/UAV binding, texture sampling,
  32-bit atomics, indirect dispatch bounds, and append/consume counters. The
  direct ABI reserves compute buffer slot 30 and graphics buffer slot 25 for
  one external structured-UAV counter; it rejects shaders that also require
  the corresponding SRV `t14`/`t9` slot or more than one counter binding.
  Exact compute append readback is `{100,101,102,103}` with counter `4`; exact
  consume readback is `{200,201,202,203}` with counter `0`. A vertex draw
  appends `{10,11,12}` with counter `3`, and a 2x2 pixel draw appends exact
  fragment values `{0,1,2,3}` with counter `4`. The shader corpus
  independently compiles and links its
  `AppendStructuredBuffer` case. Multi-counter and dynamically indexed
  counter heaps remain fail-closed. Previously silent sample-position,
  sample-count, and cycle-counter placeholders now increment the unsupported-intrinsic
  diagnostic and reject PSO creation rather than succeeding with fake values.
  Unknown/non-DXIL call sites likewise reject instead of becoming zero-valued
  temporaries until helper-function lowering is implemented.
- The LLVM 3.7 DXIL metadata reader now resolves the named `!dx.resources`
  graph into class, register-space, range, resource-kind, structured stride,
  sample-count, and UAV-flag records. The typed lowerer uses those records for
  dimension-aware MSL declarations and coordinates. The source-staged texture
  matrix passes exact readback for 1D, 1D-array, 2D, 2D-array, 3D, cube,
  cube-array, and 2D-MS resources (including dimension-specific sample/load
  coordinates and GetDimensions width/height/depth/array/sample results), plus
  UAV stores for 1D, 1D-array, 2D, 2D-array, and 3D. D3D12 1D resources use
  height-one Metal 2D/2D-array backing, avoiding Metal's one-mip 1D-array
  restriction while preserving the D3D descriptor shape. Nonzero mip-1 sample
  and GetDimensions readbacks are exact for both scalar and array 1D textures. The focused matrix also
  proves R32, R16, RG16, and RGBA8 typed families: signed/unsigned integer and
  R16 floating reads plus writable R32 and RGBA8 signed/unsigned stores all
  return exact packed values. R16 UNORM/SNORM, RGBA8 SNORM,
  R10G10B10A2 UINT/UNORM, and R11G11B10 FLOAT reads and writes also match
  their exact normalized results or packed storage bits. Scalar signed and
  unsigned 64-bit texture accesses over R32G32_UINT preserve both 32-bit
  halves for reads and writes. The D3D12 MSAA SRV view path now preserves the
  multisample Metal texture type instead of creating an incompatible 2D view.
- The shader diagnostic probe proves malformed DXIL is rejected with a
  stage-specific `shader/bitcode_parse` diagnostic and no PSO object, while
  valid DXBC/DXIL caches and D3DCompile/DXC provenance remain observable. The
  generated-report audit also checks every focused report for nonzero
  unsupported-intrinsic/opcode counts and placeholder lowering markers. The
  typed lowerer now preserves an explicit DXIL i32 destination when LLVM type
  ID zero is used by a float bitcast, emitting `as_type<int>` rather than a
  numeric conversion; it also handles the signed/unsigned result semantics
  of `firstbitlow`/`firstbithigh`. The LLVM type reader now resolves vector
  element references from `type_refs` and rejects unsupported array values
  without recursive type resolution. The source-staged semantic run continues
  to match `math_bits`, `math_intrinsics`, and all 20 semantic lanes, including
  the four-lane vector aggregate shuffle.
- Bounded descriptor indexing selects both `ByteAddressBuffer[2]` and
  `StructuredBuffer<uint>[2]` resources through generated direct-buffer
  helpers. Scalar structured loads retain their vector result type until DXIL
  extraction, avoiding the former `device uint4`-to-`uint` MSL mismatch.
- The object-contract probe now exercises pipeline-library serialization plus
  memory and disk shader-cache sessions. It verifies serialized-size/header
  round-trip, malformed-blob rejection, missing-name rejection, descriptor,
  store, size-query, short-buffer, replacement, missing-key,
  pointer-validation, and cross-session disk persistence semantics.
  `pipeline_library_serialization_pass`, `pipeline_library_recreation_pass`,
  `shader_cache_session_pass`, and `shader_cache_disk_session_pass` are true.
  The recreated stored compute pipeline dispatches and returns exact value 42;
  complete compiler-factory/state-database behavior remains in the exhaustive
  row.

## Exact evidence

The focused source-staged commands are:

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --semantic-only

DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --shader-corpus-only

DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
METAL_SHADER_CONVERTER=/nonexistent \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh \
    --texture-dimensions-only
```

The latest isolated semantic result (profile
`phase5-vector-type-fix2`) passed with these exact lanes. It was run
against the rebuilt typed lowering path with no offline shader converter,
so PSO creation exercised the runtime MSL compiler directly:

```json
{
  "ok": true,
  "math_intrinsics": {"expected": [1065353216, 0, 1065353216, 0, 0, 0, 0, 0, 1090519040, 1077936128, 1056964608, 2, 1061158912, 4294967294, 3, 3, 7, 4, 35, 8], "actual": [1065353216, 0, 1065353216, 0, 0, 0, 0, 0, 1090519040, 1077936128, 1056964608, 2, 1061158912, 4294967294, 3, 3, 7, 4, 35, 8]},
  "special_float": {"expected": [1, 1, 1, 0], "actual": [1, 1, 1, 0]},
  "atomic_uav": {"expected": [7, 0, 5, 8], "actual": [7, 0, 5, 8]},
  "sm67_vector_int64": {"expected": [68, 69, 70, 71], "actual": [68, 69, 70, 71]},
  "sm68_vector_arithmetic": {"expected": [68, 136, 204, 272], "actual": [68, 136, 204, 272]},
  "sm69_integer_float_mix": {"expected": [69, 70, 72, 73], "actual": [69, 70, 72, 73]},
  "control_flow_aggregates": {"expected": [120, 212, 328, 320], "actual": [120, 212, 328, 320]},
  "loop_aggregate": {"expected": [1, 3, 6, 10], "actual": [1, 3, 6, 10]},
  "double_arithmetic": {"expected": [37], "actual": [37]},
  "atomic_matrix": {"expected": [13, 10, 6, 15, 15, 15, 9, 15, 3, 4294967295, 3, 0, 3, 4294967295, 3, 0, 9, 5], "actual": [13, 10, 6, 15, 15, 15, 9, 15, 3, 4294967295, 3, 0, 3, 4294967295, 3, 0, 9, 5]},
  "vector_shuffle": {"expected": [1234, 2345, 3456, 4567], "actual": [1234, 2345, 3456, 4567]},
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
closed semantic, WaveOps (including active/prefix bit counts), control-flow,
double-arithmetic, atomic-binop, diagnostic, atomic/special-float,
binding-baseline, resource-metadata/texture-dimension, and focused
lowering-report-audit rows
while keeping the exhaustive SM5.x–SM6.9 opcode/stage/resource/cache/session
row open. The latest isolated
texture-dimension result is profile
`phase5-graphics-cmp1`: 66/66 cases passed with exact
dimension-specific sample/load/store and GetDimensions readback (64/96 values
for distinct slices/faces), including R32_UINT/R32_SINT `0x281e140a`,
R16_UINT `0x1234`, R16_SINT `0xfffffffe`, RG16_UINT `0x56781234`,
RGBA8_UINT `0x281e140a`, RGBA8_SINT `0xfcfdfeff`, and R16_FLOAT half bits `0x3400`. Writable typed cases return
R32_UINT `0x12345678`, R32_SINT `0xffed2979`, RGBA8_UINT `0x281e140a`, and
RGBA8_SINT `0xfcfdfeff`. The expanded normalized/packed cases return exact
read values `64` or packed UINT `0x031e140a`, and exact writable bits R16_UNORM
`0x4000`, R16_SNORM `0x2000`, RGBA8_SNORM `0x7f000020`, R10G10B10A2_UINT
`0xc1e0500a`, R10G10B10A2_UNORM `0xc0000100`, and R11G11B10_FLOAT `0x340`.
The signed/unsigned 64-bit cases preserve exact low/high pairs
`{0x89abcdef,0x01234567}` and `{0x12345678,0xffffffff}` respectively on both
load and store, with no offline converter. The height-one 2D backing gives
1D shaders Metal's complete 2D sampling modifiers while retaining D3D12 1D
coordinates and dimensions: mip-1 1D and 1D-array values are both `96`, with
exact packed
GetDimensions values `131074` and `131586`. The combined mip-level, bias,
gradient, and static-offset lane returns packed exact value `0x14323232`.
A pixel-stage derivative lane returns exact float bits `0x3f800000` for both
clamped and unclamped `CalculateLevelOfDetail`, proving the logical 1D-to-Metal
2D coordinate adaptation without relying on undefined compute derivatives.
The same draw binds a regular sampled texture and a depth-comparison texture
simultaneously; implicit and level-zero PCF sum to exact `1.0f`, proving
per-slot depth typing and graphics-stage comparison sampling.
Clamp, wrap, mirror, border, and mirror-once address modes return distinct exact
packed values, while transparent black, opaque black, and opaque white border
colors return `0x00000000`, `0xff000000`, and `0xffffffff`. Dynamic sampler
creation maps all three Metal-representable D3D border colors explicitly.
An unrepresentable red border descriptor remains fail-closed: compute dispatch
is rejected and preserves the exact two-word `0xdeadbeef` output sentinel;
the render and mesh binding paths carry the same invalid-descriptor rejection.
All eight point/linear min/mag/mip permutations plus anisotropic filtering
return distinct exact packed values from controlled magnification,
minification, and fractional-LOD samples. Minimum and maximum reduction
samplers, which Metal cannot represent, preserve the same exact sentinel
instead of silently becoming linear filters. Comparison filter bitfields retain their encoded min/mag/mip modes, and
compute depth comparisons use native Metal `sample_compare` rather than an
unfiltered read/compare approximation. Profile `phase5-gather-final` passes
all SM6.6/6.7 lanes with exact comparison-level readback `[25,75,1,1]`, exact
point/linear/static-offset filtering `[0,128,255,0]`, and exact value `1` for
the newer comparison gradient/bias lanes. Profile `phase5-cmp-dims3`
additionally returns exact float bits
`[0x3f800000,0x3f800000,0x3f800000,0x40400000]` for depth 2D-array, cube,
cube-array, and their sum. Depth SRVs now materialize Depth32 Metal views with
the requested array/cube type instead of binding an incompatible underlying
2D-array object. Native color gather and static-offset gather return
`[40,50,50,40]` and `[50,60,60,50]`; native comparison gather and
comparison-gather offset return `[0,255,255,0]` and
`[255,255,255,255]`. Broader comparison address and non-pixel graphics-stage
combinations remain in the exhaustive matrix. Resource profile
`phase5-texture1d-resource` also passes the full 108-format/shape matrix,
zero-mip normalization `{5,5,3}`, and a five-mip 1D-array creation without a
Metal validation assertion. The latest SM6.6/6.7 profile
`phase5-structured-dynamic1` additionally returns exact
`[103,203,303,403]` from both raw and structured dynamically indexed SRV
arrays. Profile `phase5-atomic-load-final` also passes every focused case,
including atomic
barrier readback `[4, 5, 6, 7]`, programmable offsets `[300, 341, 382, 383]`,
and static offsets `[260, 300, 340, 380]`, with `METAL_SHADER_CONVERTER` set to
`/nonexistent`. Profile `phase5-append-consume5` passes the complete compute
PSO probe, including exact append and consume data plus external counter
readback. Profile `phase5-graphics-counter1` additionally passes exact vertex-
and pixel-stage append cases. Profile `phase5-counter-corpus-negative` passes the shader corpus
with `append_counter_link=true` and rejects a two-counter shader with exact
`0x80004005`; `phase5-counter-semantic` preserves all 20
semantic lanes with zero mismatches.

## Remaining Phase 5 work

The complete exit gate is not claimed. The stable corpus still needs positive
and negative behavior evidence for every declared DXIL opcode/intrinsic,
control-flow and aggregate shape, graphics stage, remaining texture sampling
forms, typed/raw/structured/counter resource, cache/compiler-session path, and
all legal SM5.x–SM6.9 operations. Multi-counter, directly indexed counter-heap,
non-compute/vertex/pixel stages, and other explicitly limited providers remain
fail-closed until their exact
readback matrices pass. `D3D12_FEATURE_SHADER_MODEL` therefore remains at the
behavior-backed 6.7 report; compiling an isolated 6.9 lane does not promote a
full 6.9 capability claim.
