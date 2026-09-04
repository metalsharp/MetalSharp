# Phase 7 staged ABI checkpoint

Inspected source revision: `ce695fdf` (bounded Work Graph compute-queue probe).
This checkpoint is **not** a Phase 7 exit, clean-source build attestation, or
full-surface promotion.

## Clean-source queue and raw-UAV follow-up (`cce1ca8d`)

All nine runtime targets were rebuilt incrementally in the external Meson build
and staged from clean commit `cce1ca8ddb730e332557b8464a6d939cf1027662`.
`stage-phase6-sandbox-phase7-clean-queue.json` reports `ok=true`,
`failure_count=0`, and `source_dirty=false`. The separate strict prefix/bridge
ABI audit passes. This is a clean-source staging checkpoint, **not** an
independent repeated clean-build reproducibility proof.

The committed Work Graph probe and ordinary queue regression pass against
`/Volumes/AverySSD/metalsharp-phase7-source.tjzA3h/clean-queue/runtime`.
Exact dependency readbacks are:

- First single-input cycle: `[125, 1464291884]`.
- Repeated single-input cycle on the same compute queue: `[236, 1464292027]`.
- Multi-input dispatch form with one GPU-produced record: `[347, 1464292106]`.

The raw UAV table test and short/null-view rejection checks also pass. The
multi-input payload is copied on the GPU after queued dependencies; dispatch
metadata is still host-authored and host-read. Neither this bounded proof nor
the queue's increased command-buffer capacity closes general graph scheduling,
GPU-generated metadata, or unbounded dependency-depth requirements.

Evidence beneath `/private/tmp/metalsharp-phase7-abi/`:

- `clean-queue-stage/stage-phase6-sandbox-phase7-clean-queue.json`
- `clean-queue-stage/winemetal-abi-phase7-clean-queue.json`
- `clean-work-graph/probe-workgraph-execution-metalsharp.json`
- `clean-work-graph/probe-workgraph-metalsharp.json`
- `clean-queues/probe-queues-metalsharp.json`
- `clean-queue-build.log`

A separate scratch-only two-input, mixed-stride experiment passes with
`[347, 1464292106, 348, 1296977439]`; its source and results are
`probe-mixed-strides.cpp` and `mixed-strides/` under that scratch directory.
It is **not** part of the committed required probe matrix. The generated probe
executable was subsequently rebuilt from committed source for the clean
checkpoint above. A follow-up scratch test gives the second input a distinct
GPU-produced value at byte offset 8, retaining the 4-/8-byte stride difference;
`probe-mixed-offsets.cpp` and `mixed-offsets/` record the passing exact result
`[347, 1464292106, 1348, 1296976391]` against the clean sandbox. This additional
routing/offset witness is also scratch-only, not a required matrix promotion.

## Independent rebuild follow-up (`cce1ca8d`)

A fresh Meson build directory, `rebuild-independent`, rebuilt all nine runtime
targets with the same cross file, LLVM 15 path, Wine install path, and enabled
NVAPI/NVNGX options. No object files from the incremental build were reused.
The independently staged sandbox records the same clean source commit with
`source_dirty=false`; strict ABI and the committed Work Graph and queue probes
pass against its own runtime and Wine loader alias.

Raw artifact SHA-256 hashes differ for all nine files: **byte reproducibility
has not passed**. Section comparison finds:

- All eight PE DLL `.text` sections are identical.
- PE section differences are limited to debug sections and `.edata` bytes 4/5
  (within the export-directory timestamp). PE COFF timestamps also differ.
- All parsed Mach-O sections, including `__TEXT,__text`, are identical; the
  bridge UUID differs. Whole-file metadata differences remain unnormalized.

This establishes a second build's bounded execution/ABI result, not a release
reproducibility gate or a claim that every byte difference has been explained.
The pending checkpoint documentation was preserved in a scratch patch and
briefly reverted for clean-source staging, then restored without runtime edits.

Evidence beneath `/private/tmp/metalsharp-phase7-abi/`:

- `rebuild-independent-setup.log`, `rebuild-independent-build.log`
- `rebuild-independent-comparison.json` (raw hashes and section comparison)
- `independent-stage/stage-phase6-sandbox-phase7-independent.json`
- `independent-stage/winemetal-abi-phase7-independent.json`
- `independent-work-graph/probe-workgraph-execution-metalsharp.json`
- `independent-work-graph/probe-workgraph-metalsharp.json`
- `independent-queues/probe-queues-metalsharp.json`

The independent sandbox is
`/Volumes/AverySSD/metalsharp-phase7-source.tjzA3h/independent-sandbox`.
No generated build or probe artifacts are tracked.

## Unresolved real node input-record consumption

A separate scratch HLSL shader reads `DispatchNodeInputRecord<RECORD>` and
writes `0xabcdef00 + input.Get().value` plus marker `0x12345678` to u0.
Pinned DXC compilation succeeds and the runtime returns `hr=0x00000000`, but:

| CPU input record | Expected first output | Actual first output |
| --- | --- | --- |
| 1 | `0xabcdef01` | `0xabcdef67` |
| 9 | `0xabcdef09` | `0xabcdef67` |

The same backing-derived value 103 is read in both cases. This is a correctness
failure, not successful support or a fail-closed rejection. Existing constant
output node fixtures do not exercise submitted input-record consumption.

Source inspection identifies the missing connection:
`EncodeWorkGraphNodeShader` receives node index and record count but not the
submitted input buffer/offset or owned CPU payload. The lowering's
`GetNodeRecordPtr` uses buffer 30 (graph backing), with an output-buffer fallback;
input and output handles share the same bounded pointer helper. A future fix
must distinguish input-record storage from output-record allocation and preserve
GPU dependency ordering, resource lifetime, input bounds, and existing output
record semantics. General record scheduling remains open.

Scratch evidence under `/private/tmp/metalsharp-phase7-abi/`:
`node-input-read.hlsl`, `node-input-compile.log`, `probe-node-input.cpp`,
`probe-node-input-nine.cpp`, `node-input-before/`, and `node-input-nine/`.
The runtime is the independently staged `cce1ca8d` build. The generated probe
executable was restored from committed source after these diagnostic tests.
No runtime fix is claimed in this checkpoint.

### Source audit: full record-model dependencies

Pinned DXC's `DxilMetadataHelper.h` identifies node properties under
`dx.entryPoints`: launch type (13), program-entry flag (14), node ID (15), local
root index (16), shared input (17), dispatch grid (18), recursion depth (19),
inputs (20), outputs (21), and max grid (22). Input/output descriptors carry
flags, record types, record/output limits, array sizes, and sparse-node policy;
record type metadata carries size, SV_DispatchGrid layout, and alignment.
`node-input-dump.txt` preserves the compiled test shader's size/alignment 4/4.

Current source does not provide the full corresponding behavior:

- `GetEntrypointRecordSizeInBytes` and `GetEntrypointRecordAlignmentInBytes`
  return fixed 16 rather than the selected entrypoint's record metadata.
- `LowerWorkGraphNodeShader` returns MSL text; the program registry retains
  strings rather than the node's record/scheduling metadata.
- `EncodeWorkGraphNodeShader` fixes the Metal threadgroup shape to `{1,1,1}`.
- The lowering initializes input-record count to 1 and recursion budget to 32,
  regardless of dispatched record count or graph recursion configuration.
- `OutputComplete` sets a thread-local variable; output-count bookkeeping is
  also thread-local, with no downstream scheduler consumption at those sites.
- `FinishedCrossGroupSharing` returns literal true; node barrier variants lower
  to a threadgroup-memory barrier, not a general device-record sharing protocol.

These are explicit implementation blockers, not additional passing opcode
semantics. A complete design must retain per-entrypoint metadata through
parsing/lowering, graph construction and registry lookup; derive properties and
backing layout from it; distinguish input and allocated output records; and
publish/consume GPU work with proper record lifetime, routing, recursion,
overflow and visibility. Tests must exercise dependent consumer shaders and
actual input data across launch modes, not just constant-output witnesses.
The existing Phase 7 exit stays open pending these behaviors.

## Metadata retention implementation checkpoint

The parser now transfers its owned metadata graph and named roots into
`LLVMModule` after parse-time resource recovery. Tuple operands retain their
nullable one-based reference encoding; named roots retain zero-based IDs.
A bounds-checked module accessor handles missing/null/out-of-range operands.
This preserves forward references and per-entrypoint metadata without guessing
sizes or selecting the first entrypoint's properties for every node.

The source-owned `node_input_records.hlsl` fixture and `test_node_metadata.cpp`
verify scalar/vector input sizes 4/16, alignment 4, distinct threadgroup sizes
1/4 and dispatch grids 1/2, and ownership after module copying. The same test
fails against the previous parser (`entrypoint metadata lost`). Reproduction
instructions are in `vendor/dxmt/tests/dxil/node-metadata.md`.

All nine runtime targets build, the separate strict ABI audit passes, and the
committed Work Graph regression passes against the matching `node-metadata`
sandbox. Evidence is under `/private/tmp/metalsharp-phase7-abi/` in
`node-metadata-stage/`, `node-metadata-work-graph/`, and
`node-metadata-final-build.log`. This stage has a dirty source snapshot.
The host test needed an exception for the pre-existing unused parser constant
warning; other enabled warnings remained errors.

**This does not yet change runtime record size/alignment queries, input-record
binding, output allocation, or GPU scheduling.** Those blockers remain open;
metadata retention is an implementation prerequisite, not Phase 7 completion.

## Invalid entrypoint-layout query contract

The [Work Graph specification](https://microsoft.github.io/DirectX-Specs/d3d/WorkGraphs.html#getentrypointrecordsizeinbytes)
requires `UINT_MAX` for invalid graph/entrypoint indices in record size and
alignment queries; zero denotes valid empty input, not an invalid index.
Both runtime methods now return the required invalid-index sentinel.
The existing required properties probe includes out-of-range graph and
entrypoint indices, including `UINT_MAX`. It fails against the preceding
runtime and passes after the correction, with successful dispatch HRESULTs
in both runs. Evidence: `/private/tmp/metalsharp-phase7-abi/query-before/`,
`query-after/`, and `query-stage/` (separate strict ABI audit).
This changes only the invalid-index contract: valid record layouts remain
hard-coded and must still be derived from the retained entrypoint metadata.

## Metadata-derived entrypoint layouts

`nodeInputLayout` now decodes each named node's input layout from retained DXIL
metadata. Shader-backed state objects store those layouts per node; property
queries resolve entrypoint identity to its node rather than assuming matching
array order. Nonempty records meet the DWORD minimum size/alignment; empty
inputs return zero, while invalid indices retain `UINT_MAX`.

The required `node_input_layouts_exact` probe builds a four-node library and
reorders the entrypoint list. It checks vector `(16,4)`, empty `(0,0)`, scalar
`(4,4)`, and 16-bit `(4,4)` size/alignment pairs. The source-owned host test
also checks missing/duplicate entrypoints and module ownership. Existing
output-only node fixtures now correctly expect zero input size/alignment.
The library-free reference fixture retains its synthetic `(16,16)` layout;
its previously mismatched entrypoint names were corrected to identify its nodes.

The new queries fail against the previous runtime (`layout-before/`) and pass
after the change (`layout-final/`), along with the complete Work Graph
regression. Strict ABI evidence is in `node-layout-stage/`, all beneath
`/private/tmp/metalsharp-phase7-abi/`. The sandbox records dirty-source development
provenance. This does not fix actual input-record binding or GPU scheduling;
node-ID overrides, full library discovery and other graph semantics remain open.

## Entrypoint-to-shader execution routing

Program registration now resolves each entrypoint's node identity and stores
shader text in entrypoint order, matching DispatchGraph's index namespace.
Previously the registry used node-array order, even though property queries
correctly resolved entrypoint identities. Unresolved entries retain empty slots
rather than shifting subsequent indices.

The source-owned three-node execution fixture keeps nodes `[a,b,c]` but declares
entrypoints `[b,c,a]`. Final CPU dispatch and one-input multi-CPU dispatch to
entrypoint 2 now observe node a's `[0x11111111,0xaaaa0001]`; one-input multi-GPU
dispatch to entrypoint 0 observes node b's `[0x22222222,0xbbbb0002]`.
The same probe fails on the preceding runtime and passes with registration
remapping. Evidence is under `/private/tmp/metalsharp-phase7-abi/route-before/`,
`route-after/`, and `node-route-stage/` (strict ABI audit).
This is shader-selection evidence, not input-payload consumption, node-ID
customization, fan-out, or general GPU scheduling. Registry-side input-layout
transport remains to be implemented alongside actual record binding.

## Owned registry-side input layouts

Registered entrypoint programs now own a `WorkGraphNodeShader` value containing
MSL text plus decoded input record size/alignment. Registration copies layouts
in the same resolved entrypoint order as shader text; lookup copies the complete
value under the registry mutex and clears the output on a miss. Command replay
receives this value instead of a bare string. It does not retain pointers into
state-object vectors or temporary LLVM modules.

All nine runtime targets build; strict ABI and the complete Work Graph
regression pass against the matching `node-registry` sandbox. Evidence:
`/private/tmp/metalsharp-phase7-abi/registry-build.log`, `node-registry-stage/`,
and `registry-after/`. This is metadata transport and a regression checkpoint,
not proof of shader input binding: the encoder does not yet use these sizes to
bind records or implement scheduling. Those execution blockers remain open.

## Actual single-record input execution

Input record handles are now distinct from output handles in lowering. A
versioned 32-byte internal input context describes count, stride, record size,
and byte length; it is bound separately from the input payload and graph
backing/output records. CPU input is copied from the command's owned record
bytes to retained transient storage. GPU input binds the retained resource
and offset directly, so queued waits protect the shader read rather than a
premature host snapshot. Input pointer arithmetic checks the context version,
index and byte bounds. Output-record allocation still uses the previous bounded
provider and is **not** a general allocator or scheduler.

The source-owned test now proves:

- CPU values 1 and 9 yield `0xabcdef01` and `0xabcdef09`, with marker
  `0x12345678`, despite mutation of the original CPU value after recording.
- GPU value 37 yields `0xabcdef25`, distinguishable from the preceding CPU
  result, preventing an unchanged-output false positive.
- A real input-reading node submitted before its GPU producer, using queue
  Wait/Signal and no intervening host completion wait, observes value 457 and
  returns `[2882400457,305419896]`.
- A 4-byte record supplied to the 16-byte vector input and an unaligned GPU
  input address leave the output unchanged.
- A shader declaring user u28 is rejected with `E_FAIL` and null state output,
  rather than aliasing the internal input context. Current user node resources
  are limited to the actually resolved counter-free u0/space0 buffer binding;
  general node argument tables remain to be implemented.

The input-consumption probe fails on the preceding runtime and passes with the
new binding. All nine targets build, the full Work Graph and ordinary queue
regressions pass, and strict ABI passes against matching development artifacts.
Evidence under `/private/tmp/metalsharp-phase7-abi/`: `input-binding-before/`,
`input-final-work-graph/`, `input-final-queues/`, `input-binding-final-stage/`,
and `input-binding-final-build.log`. This stage records dirty source provenance.

The execution candidate is still one record; this checkpoint does not establish
multiple records, all launch shapes, custom node IDs, downstream publication,
recursion, device-wide sharing/barriers, or GPU-generated dispatch metadata.
Phase 7 remains open pending those behaviors.

Manual review also found that real programs outside the single-record candidate
could fall through to the synthetic reference kernel. The registry guard now
covers those shapes before reference execution. A two-record real-node dispatch
checks both output and backing memory for unchanged data; it fails before the
guard and passes afterward. Evidence: `input-shape-before/`,
`input-guarded-after/`, and `input-guarded-stage/` under the same scratch root.
This explicitly rejects an unimplemented shape; it does not implement it.

## Fixed launch geometry and complete GEP index parsing

Node metadata now retains launch type, threadgroup dimensions and fixed dispatch
grid alongside input layout, through state construction and program registration.
Command replay uses those dimensions rather than one thread/one group. A source-
owned broadcasting node with `NumThreads(4,1,1)` and `NodeDispatchGrid(2,1,1)`
reads a submitted uint4 and produces all eight exact values:
`[101,202,303,404,101,202,303,404]`. Its CPU input array is mutated after recording.

The test exposed a second defect after the launch fix: all eight lanes initially
read component zero. GEP bitcode decoding incorrectly advanced by fixed pairs,
although type IDs are present only for forward references. Parsing now uses
LLVM/DXC's value/type-pair rule for every operand. The host test checks that the
vector GEP retains base plus all three indices; the runtime test verifies the
dynamic component selection. This does not make all nested-record byte-offset
arithmetic type-aware; arbitrary layouts and non-32-bit indexed aggregates
remain to be verified and implemented.

Record-driven `SV_DispatchGrid` requires separate runtime decoding. A node with
only `NodeMaxDispatchGrid` is rejected with `E_FAIL` and null state output, not
launched with an invented one-group grid or with the maximum as its actual grid.
The host decoder tests the same boundary. Other launch modes and multi-record
scheduling are not certified by this fixed broadcasting witness.

Evidence beneath `/private/tmp/metalsharp-phase7-abi/`:

- `launch-before/`: one-thread failure.
- `launch-after/`: all lanes read 101 before GEP parsing was fixed.
- `launch-vector-after/` and `launch-final/`: exact eight-lane success and final
  dynamic-grid rejection proof.
- `node-launch-vector-stage/`: separate strict ABI audit of matching artifacts.
- `launch-compute-pso/`, `launch-reflection-abi/`, and `launch-graphics/`:
  passing compute, reflection, and graphics regressions.
- `launch-shader-corpus/` and `launch-corpus-before/`: both fail only the existing
  `two_counter_fail_closed` case; all case outcomes match. The corpus gate has
  **not** passed, and title-capture gating remains false.
- `launch-vector-build.log`: all nine runtime targets built.

These are development snapshots, not a clean release or Phase 7 exit.

## Typed record offsets and cross-block pointer lifetime

GEP instructions now retain their explicit source element type, and LLVM struct
types retain the packed flag. Node record pointers and derived GEP chains use
checked scalar-aligned struct/array layout arithmetic rather than scaling every
index by four. This is scoped to node record storage; existing Metal private
scratch addressing is not silently reinterpreted as a different LLVM layout.
Packed/unsupported type forms, cycles, invalid IDs and layout overflow fail
layout recovery instead of receiving invented offsets.

A source-owned 48-byte input record contains a uint4 prefix and a nested tail
with four uint16 values, a uint16 spacer, an alignment gap, and two uint64 values. Twelve threads read
its members through separate control-flow blocks. Expected readback is:
`[101,202,303,404,5,32768,65535,4097,287454020,1432778632,2578103244,3723427584]`.
The host test verifies root offsets `[0,16]`, nested offsets `[0,8,16]`
(absolute offsets `[0,16,24,32]`), size 48 and alignment 8;
the CPU record is mutated after command recording.

This exposed two additional lowering defects: the input record pointer was
block-local despite uses in other dispatch-state blocks, and i16 zero-extension
used signed widening. Record pointer results now receive kernel-scope
predeclarations in control-flow dispatch, and i16 ZExt casts through ushort
before widening. The high-bit uint16 values and upper/lower uint64 words pass
without weakening the expected values.

Evidence under `/private/tmp/metalsharp-phase7-abi/`:

- `offset-before/`, `offset-after/`: failures before the full offset/CFG fixes.
- `offset-cfg-after/`: initial mixed-width record success.
- `offset-padded-final/` and `offset-nested-final/`: padded and then nested-record
  success, including full Work Graph passes.
- `node-offset-cfg-stage/`: strict ABI passes with matching development artifacts.
- `offset-compute-pso/`, `offset-graphics-pso/`, `offset-reflection-abi/`: pass.
- `offset-shader-corpus/`: same case outcomes as the baseline, still failing
  `two_counter_fail_closed`; this is not a passing corpus gate.
- `offset-cfg-build.log`: all nine runtime targets built.

These remain dirty-source development proofs. Pointer transformations beyond
tracked record/GEP chains, arbitrary IR type forms, multiple input records,
record publication and the complete GPU scheduler remain open.

## Original observations

- The staged `dxmt_m12` bridge passed the Winemetal export/source-layout audit
  (contract ABI version 31). The staged and Wine builtin x64 bridge hashes matched.
- A newly created disposable Wine prefix lacked `system32/winemetal.dll`.
  The strict prefix audit failed, despite the focused Work Graph probe passing.
- `run-probes.sh --work-graph-only` disables the ABI audit internally. Omitting
  `--no-winemetal-abi` does not make this focused invocation an ABI gate.
- After copying the staged x64 `winemetal.dll` into the disposable prefix's
  `system32`, the strict audit passed with `ok=true`, `failure_count=0`.
  The existing syswow64 bridge passed its legacy-export checks.
- A subsequent Work Graph execution probe passed with `hr=0x00000000`, all
  required readback fields true, `gpu_native_provider=true`, and
  `cpu_scheduler=false`.
- `cross_queue_dispatch_exact` covers one CPU-input record submitted on a compute
  queue after host completion of preceding direct-queue work. It does **not**
  establish inter-queue GPU fence ordering or three-record compute dispatch.

Audited x64 hashes:

| Artifact | SHA-256 |
| --- | --- |
| winemetal.dll | `2707a77c9043f0ef0785c37f38b36f11c502df7f50d85d3b80125ffd0033fd8f` |
| winemetal.so | `5ca20fd6523a5b2888e5936535ec7dc7d94ab997eb2d692d947537f3bd3b20cf` |

## Reproduction

From the repository root, use a **disposable, already initialized** Wine prefix
and the matching staged runtime. Do not apply these instructions to a user's
Steam or game prefix.

```sh
runtime="$HOME/.metalsharp/runtime/wine/lib/dxmt_m12"
# Set prefix and results to dedicated scratch directories.
cp "$runtime/x86_64-windows/winemetal.dll" \
  "$prefix/drive_c/windows/system32/winemetal.dll"
python3 tools/d3d12-metal-sdk/scripts/check-winemetal-abi.py \
  --profile phase7-prefix-staged --dxmt-runtime "$runtime" \
  --wine-runtime "$HOME/.metalsharp/runtime/wine" \
  --prefix "$prefix" --results-dir "$results/abi"
WINEPREFIX="$prefix" \
DXMT_PROBE_DLL_OVERRIDES='d3d12=n,b;dxgi=n,b;d3d11=n,b;d3d10core=n,b;winemetal=n,b' \
  tools/d3d12-metal-sdk/scripts/run-probes.sh \
  --profile metalsharp --dxmt-runtime "$runtime" --work-graph-only \
  --results-dir "$results/workgraph"
```

Inspect JSON `ok`/`pass` fields, not just the runner's exit code. Local evidence
for this checkpoint is under `/private/tmp/metalsharp-phase7-abi/`: strict audit
in `run/winemetal-abi-phase7-prefix-staged.json`, and post-audit execution in
`verified/probe-workgraph-execution-metalsharp.json`. Scratch evidence is not a
committed release artifact.

## Remaining boundaries

The staged runtime manifest identifies a bundled source rather than providing a
clean-source build attestation for this checkout. Matching bridge hashes and a
source-layout audit do not prove that every staged binary was built from current
source, nor do they establish all Windows-side ABI layouts or device execution
semantics. The fresh-build follow-up below addresses this checkpoint's source-staging gap;
repeated-build reproducibility, broader regressions, all remaining phase exit
gates, and release/promotion gates remain required.

## Fresh-source sandbox follow-up

Built revision `fd91fb9f2c5eb7f4f0403351e407236d33e70837` in a new external
Meson directory, not the retained tracked build tree. Configuration used
`vendor/dxmt/build-win64.txt`, pinned x86 LLVM 15.0.7, the installed Wine toolchain,
and `-Denable_nvapi=true -Denable_nvngx=true`. Built the nine runtime targets
listed by `prepare-dxmt-x86-llvm15.sh`, then staged with
`stage-phase6-sandbox.py --profile phase7-source` (its schema name remains Phase 6).

- Staging: `ok=true`, `failure_count=0`, `source_dirty=false`.
- Strict prefix/bridge ABI audit: `ok=true`, `failure_count=0`.
- Fresh-source Work Graph execution: `pass=true`, `hr=0x00000000`.
- Fresh-source bounded video processing regression: `pass=true`.
- No runtime capability was promoted.

Fresh bridge hashes differ from the installed bundle and are recorded separately:

| Artifact | SHA-256 |
| --- | --- |
| winemetal.dll | `abf1b7d06210ecea885bcc2b7e8e48920b66507e34668b6a58acb4063875fb98` |
| winemetal.so | `f5f48834acc69596cbb89625accb62a61c7121b4de7c35ef4d4903e167445562` |

The first sandbox invocation failed before producing execution JSON: it unloaded
the selected D3D12 module and faulted. The runner still returned zero. The unique
Unix bridge alias was in the sandbox builtin directory, which was absent from
`WINEDLLPATH`. Retrying with **both** sandbox route and builtin roots passed:

```sh
# sandbox is the root passed to stage-phase6-sandbox.py.
# Expose the layout expected by run-probes.sh without touching installed Wine.
ln -s ../../runtime "$sandbox/wine/lib/dxmt"
export DXMT_PROBE_WINEDLLPATH="$sandbox/wine/lib/dxmt:$sandbox/wine/lib/wine"
# Use --dxmt-runtime "$sandbox/wine/lib/dxmt" for the focused probes.
# Before execution, copy this sandbox's winemetal.dll into the disposable
# prefix's system32 and rerun check-winemetal-abi.py with
# --wine-runtime "$sandbox/wine" (no optional-prefix exemption).
```

Local evidence remains under `/private/tmp/metalsharp-phase7-abi/`:
`source-stage/` holds staging and ABI manifests, `source-path-fixed/` the passing
Work Graph result, and `source-video/` the video result. `source-workgraph/` and
`source-debug/` retain the failed loader attempts. `build.log` records the fresh
build; `build-root.txt` identifies the external build/sandbox directory. These
are scratch artifacts, not a release bundle or an exhaustive regression gate.

### Additional affected regressions on the same sandbox

Without rebuilding or changing the source snapshot, the following focused runs
also passed (JSON checked independently of shell exit status):

| Invocation | Evidence under `/private/tmp/metalsharp-phase7-abi/` | Result |
| --- | --- | --- |
| `--cpu-texture-map-only` | `source-cpu-texture-map/probe-cpu-texture-map-metalsharp.json` | `pass=true`, `exact=true` |
| `--discard-texture-only` | `source-discard-texture/probe-discard-texture-metalsharp.json` | `pass=true`, `exact_rect_zeroing=true` |
| `--reflection-abi-only` | `source-reflection-abi/probe-reflection-abi-metalsharp.json` | `pass=true`; binding reflection and deterministic mismatch rejection |
| `--mini-only`, filter `dxr_acceleration_structures` | `source-dxr/probe-mini-dxr_acceleration_structures-metalsharp.json` | `ok=true`, `hr=0x00000000` |
| `--mini-only`, filter `mesh_object_shader_pso` | `source-mesh/probe-mini-mesh_object_shader_pso-metalsharp.json` | `ok=true`, `hr=0x00000000` |

The two mini runs used `METALSHARP_NATIVE_IRCONVERTER=1` and
`METALSHARP_MINI_PROBE_FILTER` set to the listed filter. All used the explicit
sandbox `DXMT_PROBE_WINEDLLPATH`, DLL overrides, and disposable prefix described
above. Each result directory has its own host-runtime manifest and shader cache.

The DXR probe verified its bounded mixed-geometry fallback, direct/indirect ray
dispatch, and serialization/traversal cases. This does not establish arbitrary
heterogeneous BLAS support. The mesh probe verified direct/indirect dispatch,
array-layer output, depth/blend/wireframe behavior, payload-tail readback, and
pipeline statistics for its source-owned fixture; it is not general mesh DXIL
conversion evidence. Probe fields named `tier1_matrix_complete` or
`tier1_1_matrix_complete` describe those bounded probe matrices, not exhaustive
full-surface tier certification. No capability or unsupported-ledger entry was
promoted based on these runs.
