# Phase 7 staged ABI checkpoint

Inspected source revision: `ce695fdf` (bounded Work Graph compute-queue probe).
This checkpoint is **not** a Phase 7 exit, clean-source build attestation, or
full-surface promotion.

## Downstream chain development witness (uncommitted scheduler)

Follow-up: the original HelloWorkGraphs sample and the new source-owned
`probes/probe_workgraph/node_chain.hlsl` both now pass the same exact readback.
The latter uses uniform group barriers and is executed by
`probes/probe_workgraph_chain.cpp`, built and run by the official SDK scripts.
The official runner now completes: chain, existing execution matrix, and
opcode probes all report `pass=true` under
`/private/tmp/wg-chain-final/` with profile `phase7-workgraph`. Both Work Graph
contract rows match every required field, including the explicitly permuted
entrypoint readbacks (node_a for CPU/final dispatch and node_b for GPU input).
`abi/winemetal-abi-chain-final.json` passes the strict audit. Staging is still
dirty-source; none of this closes Phase 7 or release provenance.

Additional fixes preserve constant-expression GEPs instead of converting
threadgroup addresses to zero, seed checked direct-global threadgroup offsets,
separate canonical node identities from reordered entrypoint indices, and avoid
resetting backing memory before terminal-only GPU inputs have been copied.
`tests/dxil/test_constant_gep.cpp` verifies retained typed operands for four
shared addresses; the original node metadata regression also passes.
The dynamic-grid creation check now requires acceptance rather than rejection;
actual record-driven group counts are asserted by the separate chain readback.
The runner registers Unix aliases in the selected Wine executable's installation,
not the unrelated sibling directory of an external DXMT staging route.

Zero-grid follow-up: the grid builder no longer substitutes one for a zero
record grid. The regression failed before the change (the zero records still
produced `[5,2]` contribution pairs) and passes afterward. Official results in
`/private/tmp/wg-chain-zero-final2/` show mixed-grid output
`[18,0,18,0,4,0,4,0]`, all-zero output of sixteen zero words, exact allocation
counts, and continued success for the original chain, execution, and opcode
probes. The runner now captures JSON on stdout consistently for all chain modes.

Grid-layout follow-up: the GPU builder now uses DXIL record-type tag 1's
byte offset and component count, not record stride. Metadata validates U32
components (1–3), aligned offsets, and record bounds; other component types
fail closed. The source-owned offset variant places a scalar grid at byte 4
in a 16-byte record with zero padding, and produces the same exact chain
readback. All six official results pass in `/private/tmp/wg-chain-offset-final/`.
Host regressions also reject misaligned offsets, invalid component types, and
zero component counts. Multi-axis runtime grids and U16 support remain open.

Three-axis follow-up: the vector variant uses `{2,2,1}`, `{1,1,2}`,
`{2,1,1}`, and `{1,1,1}` record grids at byte offset 4. The exact chain result
is `[36,10,18,5,8,4,4,2]` with untouched zero tail, 36 allocated records and
27 allocations. All seven official Work Graph results pass in
`/private/tmp/wg-chain-vector-final/`. This closes the bounded non-unit Y/Z
witness, not U16 grids or general downstream broadcasting.

GPU-input follow-up: `gpu-vector-grid` supplies the header and records through
a separate 128-byte upload resource, with records at byte offset 64. They are
zero at `DispatchGraph` recording time and replaced with the three-axis payload
after recording, before submission. The full chain still returns
`[36,10,18,5,8,4,4,2]` and exact allocation counts; JSON independently records
GPU-input mode and successful post-recording mutation. All eight official
results, Work Graph contract checks, and strict ABI audit pass under
`/private/tmp/wg-chain-gpu-final2/`. This proves queued payload visibility, not
GPU-produced header handling or cross-queue chain dependencies. Source remains
dirty and Phase 7 remains open.

Fan-out follow-up: the entry node now has a fixture variant publishing two
distinct output IDs: one through the middle node and one directly to the final
coalescing node. Both paths contribute exactly once: `[18,5,18,5,6,3,6,3]`,
30 records and 24 allocations. A second variant uses four grids of 16 groups,
filling all 256 allocation-table entries with 320 records, and exercising
multiple `[MaxRecords(32)]` coalescing batches. It yields
`[1040,1040,1040,1040,48,48,48,48]` with unchanged zero tail. All ten official
Work Graph results pass in `/private/tmp/wg-chain-capacity-final/`.
This certifies the bounded full-table witness, not overflow handling or general
graph topology. No intermediate CPU scheduling/readback was introduced.

Topology-preflight follow-up: a new unsupported downstream broadcasting fixture
exposed partial execution: before the fix, its entry shader wrote `0xdeadbeef`
to UAV word 15 and allocated records even though the downstream edge could not
execute. The full reachable topology is now checked before backing reset or
shader encoding. It checks bounded output/input sizes, supported target launch
shapes, depth, cycles, and a 1024-node-visit encoding limit. This static
validation is not CPU scheduling. The negative fixture now leaves all sixteen
UAV words and allocation counters zero; all eleven official Work Graph results
pass in `/private/tmp/wg-chain-preflight-final/`. Runtime cycle and path-limit
probes are still pending; recursion and downstream broadcasting remain unsupported.
This does not promise rollback for unrelated allocation or pipeline failures.

Cycle-rejection follow-up: a `NodeMaxRecursionDepth(2)` fixture routes its
middle node's `Work` output back to that same node using explicit `NodeID`.
The entry shader deliberately writes a sentinel if executed. Dispatch leaves
all UAV words and allocation counters zero, verifying rejection before entry
side effects. All twelve official Work Graph results pass in
`/private/tmp/wg-chain-cycle-final/`. This is fail-closed evidence, not recursive
execution support; actual recursion-level semantics and path-budget boundary
probes remain open.

U16-grid follow-up: metadata now preserves component byte width (U16 or U32)
and validates offsets/bounds at that width. The GPU builder uses little-endian
byte loads, including grids beginning at byte offset 2. The packed U16 fixture
uses an 8-byte record and returns `[36,10,18,5,8,4,4,2]` with exact allocations.
`test_grid_metadata.cpp` checks offsets/widths/counts and copied-module ownership
for scalar U32, offset U32, vector U32 and vector U16 fixtures. All thirteen
official Work Graph results pass in `/private/tmp/wg-chain-u16-final/`.
GPU-produced U16 payloads and general downstream broadcasting remain open.

Queued-copy/cross-queue follow-up: two new modes copy the post-recording
mutated upload payload into a DEFAULT-heap record buffer and transition it for
shader reads. One runs the copy on the graph's direct queue. The other runs it
on a compute queue held behind a fence and adds a producer-fence wait to the
graph queue. The consumer completion remains pending for a bounded fence-only
check before releasing the producer; no intermediate records are read back.
Both chains return `[36,10,18,5,8,4,4,2]` and exact allocation counts. All fifteen
official Work Graph results pass in
`/private/tmp/wg-chain-cross-queue-gated-final/`. These witnesses do not prove
GPU-generated input headers or general multi-queue graph synchronization.

Self-review closeout: terminal/unconnected outputs now receive the same
256-byte slot-size check as connected edges. An unconnected 264-byte output
fixture leaves its deliberate entry-shader sentinel and allocation counters
untouched. All sixteen official Work Graph results pass in
`/private/tmp/wg-chain-review-final/`. The checkpoint remains bounded and
non-promoted: larger records, recursive execution, downstream broadcasting,
GPU-generated headers and general argument/resource tables are not certified.

Historical first witness (before these follow-ups):

A scratch three-node chain now produces exact GPU readback
`[18,5,18,5,4,2,4,2,0,0,0,0,0,0,0,0]` from four CPU input records
`{grid,index} = {2,0},{1,1},{2,2},{1,3}`. Broadcasting output feeds a thread
node, then a coalescing node; output routing and indirect counts are built on
the GPU without CPU scheduling or intermediate readback. This is dirty-source,
scratch-only development evidence, not a required-matrix or phase exit.

The experiment exposed and corrected three implementation defects:

- `SV_DispatchGrid` already contains threadgroup counts; dividing it by
  `NumThreads` dropped groups.
- Version-3 scheduled-input contexts must retain version-2 output allocation,
  record-pointer, and publication behavior.
- Group output allocation must reserve once per group and broadcast its handle,
  rather than reserve independently for every thread.

The passing fixture adapts Microsoft's HelloWorkGraphs chain by replacing the
final groupshared reduction with a per-record atomic increment. At that intermediate checkpoint the original
sample was unverified: its generated MSL contains null threadgroup load
addresses, and the original reduction produced incorrect values before the
last allocation fix. Do not conflate the adapted witness with sample support.

Evidence: `/private/tmp/wg-chain-direct/` contains `run.py`, the adapted HLSL,
`stdout` (`exact=true`), compiler/build logs, and runtime traces. Probe source
is `/private/tmp/probe-wg-chain.cpp`. Staging and strict ABI evidence are under
`/private/tmp/wg-chain-results/`; `abi/winemetal-abi-chain-group-fix.json`
reports `ok=true`, `failure_count=0`. The metadata regression, contract validator,
and whitespace gate also pass. These temporary paths are not release artifacts.
The official `--work-graph-only` runner still timed out before its D3D12 result;
the witness used explicit PE aliases and a temporary Unix alias in the actual
Wine installation, not the incomplete sibling staging Wine directory.

Remaining: expand reproducible negative chain probes, certify dynamic-grid field
layouts and overflow, fan-out/recursion/multigraph semantics, run broader shader
regressions for constant GEP fail-closed behavior, and rerun the full required
matrix and clean-source release gates.

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

## Resolved real node input-record consumption

The source-owned `node_input_records.hlsl` fixture now reads submitted
`DispatchNodeInputRecord`, `ThreadNodeInputRecord`, and
`GroupNodeInputRecords` payloads through a distinct input buffer/context ABI.
Broadcasting and thread launches dispatch once per record; coalescing launches
GPU batches at the declared `[MaxRecords(4)]` boundary and preserves dynamic
`input[index]` access. CPU records are mutated after command recording and GPU
records are supplied through descriptor addresses, so neither case can pass by
reading a host snapshot or graph backing bytes.

The same lowering now has a bounded GPU-atomic output allocation table for
sufficiently sized initialized graph backing memory. Allocated records use
256-byte slots after the table, and `OutputComplete` publishes the allocation
marker without aliasing user `u0`. The `node_records` fixture reads back the
record value and publication marker exactly.

Remaining work is intentionally separate: output IDs are not yet consumed by a
GPU downstream scheduler, recursion/fan-out and cross-group sharing are not
implemented, and record-driven grids/general node resource tables remain
fail-closed. Final evidence is under `/private/tmp/metalsharp-phase7-abi/` in
`node-final/`, `node-final-stage/`, and `node-final-abi.log`.

### Source audit: full record-model dependencies

Pinned DXC's `DxilMetadataHelper.h` identifies node properties under
`dx.entryPoints`: launch type (13), program-entry flag (14), node ID (15), local
root index (16), shared input (17), dispatch grid (18), recursion depth (19),
inputs (20), outputs (21), and max grid (22). Input/output descriptors carry
flags, record types, record/output limits, array sizes, and sparse-node policy;
record type metadata carries size, SV_DispatchGrid layout, and alignment.
`node-input-dump.txt` preserves the compiled test shader's size/alignment 4/4.

Current source does not provide the full corresponding behavior:

- `LowerWorkGraphNodeShader` returns MSL text; the program registry now retains
  input layout, launch geometry, `[MaxRecords]`, and bounded output metadata
  needed by the native node path, but not the complete graph scheduler.
- `EncodeWorkGraphNodeShader` derives threadgroup/grid dispatches from retained
  per-entrypoint metadata and uses one input context per record or coalescing
  batch.
- The lowering initializes the recursion budget to 32; configured recursion
  depth and remaining-level propagation are still open.
- `OutputComplete` publishes a GPU-visible allocation marker for the bounded
  initialized backing ABI; output-count bookkeeping and downstream consumption
  remain open.
- `FinishedCrossGroupSharing` returns literal true; node barrier variants lower
  to a threadgroup-memory barrier, not a general device-record sharing protocol.

These are explicit implementation blockers, not additional passing opcode
semantics. A complete design must retain per-entrypoint metadata through
parsing/lowering, graph construction and registry lookup; derive properties and
backing layout from it; distinguish input and allocated output records; and
publish/consume GPU work with proper record lifetime, routing, recursion,
overflow and visibility. The current tests cover actual input data across
launch modes, bounded output publication, post-recording mutation, and exact
fail-closed shapes; dependent consumer shaders, output-ID routing, and GPU
scheduler execution remain required.
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

**This checkpoint now changes runtime record size/alignment queries, bounded
input-record binding, and bounded output allocation/publication.** GPU
scheduler routing, recursion/fan-out, and the remaining general graph ABI
blockers remain open; metadata retention alone is not Phase 7 completion.

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
provenance. It does not implement GPU scheduler routing; node-ID overrides,
full library discovery and other graph semantics remain open.

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
and `registry-after/`. This is metadata transport and a regression checkpoint;
the encoder now uses these sizes for bounded input binding and launch geometry,
while graph output routing and scheduling remain open.

## Actual single-record input execution

Input record handles are now distinct from output handles in lowering. A
versioned 32-byte internal input context describes count, stride, record size,
and byte length; it is bound separately from the input payload and graph
backing/output records. CPU input is copied from the command's owned record
bytes to retained transient storage. GPU input binds the retained resource
and offset directly, so queued waits protect the shader read rather than a
premature host snapshot. Input pointer arithmetic checks the context version,
index and byte bounds. Output-record allocation now has a distinct bounded
GPU-atomic table and publication marker for sufficiently sized initialized
backing memory; it is **not** a general graph allocator or downstream scheduler.

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

The execution candidate now covers bounded multiple records for broadcasting,
thread, and coalescing launches, including declared-MaxRecords batching and
GPU/CPU input ownership. This checkpoint does not establish all launch shapes,
custom node IDs, downstream output-ID routing, recursion, device-wide
sharing/barriers, or GPU-generated dispatch metadata. Phase 7 remains open
pending those behaviors.

Manual review also found that real programs outside the earlier single-record
candidate could fall through to the synthetic reference kernel. The registry
guard covers unsupported shapes before reference execution. The current
multi-record positive cases now execute through the real lowered node shader;
unsupported record-driven grids and resource bindings still check unchanged
output and fail closed. Evidence: `input-shape-before/`, `input-guarded-after/`,
`input-guarded-stage/`, and `node-final/` under the same scratch root.

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
The host decoder tests the same boundary. The later Work Graph regression
certifies bounded thread/coalescing input execution; output-ID routing,
recursive/fan-out scheduling, and record-driven broadcasting grids remain open.

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
tracked record/GEP chains, arbitrary IR type forms, record publication and the
complete GPU scheduler remain open.

## Multiple fixed-grid input records

The source-owned node provider now accepts a bounded array of input records for
fixed-grid broadcasting nodes and thread-launch nodes. It allocates or validates
the complete input byte range, then emits one GPU compute dispatch per record
with the input buffer offset and one-record context view selected for that
invocation. Coalescing nodes instead use the declared input `[MaxRecords]` as
the batch boundary, set the context count/byte bounds for each batch, and allow
dynamic `input[threadIndex]` GEP indices to resolve through the GPU input ABI.
This preserves input-record access without a host readback or CPU shader
execution. A sufficiently sized initialized backing allocation now also
supports bounded GPU-atomic output allocation and `OutputComplete` publication.
Dynamic record grids and downstream scheduling are still rejected rather than
approximated.

`node_broadcast_multi` writes two distinct slots from two records. The CPU input
case mutates both source records after command recording and reads back slots 1
and 6 exactly. The GPU descriptor case reads two records from a GPU-addressed
buffer and reads back slots 0 and 7 exactly. `node_thread_multi` proves the same
CPU multi-record path for thread launch. `node_coalescing_multi` processes six
records with a declared `[MaxRecords(4)]`, forcing two GPU batches; both CPU
(post-recording mutation) and GPU-addressed input arrays have exact slot
readback. All cases use lowered DXIL node shaders and root `u0` binding.
The bounded input aggregate is in `/private/tmp/metalsharp-phase7-abi/node-final/`.
The separate output-allocator rerun is in
`/private/tmp/metalsharp-phase7-abi/output-allocator7/`; its final result reports
`pass=true`, `dxil_node_output_records_exact=true`, and
`node_output_record_values=[2882400001,1]` (with the intentionally false
`cpu_scheduler` witness).

## Bounded GPU output allocator/publication follow-up

The first output-allocator6 execution did not produce a result because the
shared backing buffer still contained data written by earlier probe cases; the
GPU allocator correctly rejected its stale counter values. The probe now
explicitly reinitializes the 2 MiB upload backing before the independent node
output dispatch, matching the required initialized-backing precondition rather
than adding a CPU scheduler or host-side record publication.

The current lowered `node_records` shader performs GPU atomic reservations for
one allocation and one 256-byte record slot, writes the record at byte 8192,
and publishes the table entry with `OutputComplete`. The staged result checks
record counter `1`, allocation counter `1`, table base `0`, count `1`, output
handle `1`, publication marker `1`, and record value `0xabcdef01`; user UAV
`u0` remains a separate 64-byte resource and still reads its independent
`[0xabcdef01,0x12345678]` witness.

The external source build was incrementally rebuilt and restaged as
`output-allocator7` with `source_dirty=true`; the strict PE/Unix ABI audit
passed with `ok=true` and `failure_count=0`. The generated MSL contains the
non-zero `m12_node_allocate_record_handle` call and compiles with the pinned
Metal toolchain (warnings are limited to existing unused generated variables).
The exact runtime result was captured through the explicit D3D12/Winemetal
aliases in a disposable Wine prefix.

Evidence beneath `/private/tmp/metalsharp-phase7-abi/output-allocator7/`:

- `stage-phase6-sandbox-output-allocator7.json`: synchronized staged artifacts,
  `ok=true`, with dirty-source development provenance.
- `abi-staged/winemetal-abi-output-allocator7-staged.json`: `ok=true`,
  `failure_count=0`.
- `direct-staged-final/probe-workgraph-execution-output-allocator7-direct-staged-final.json`:
  `pass=true`, `hr=0x00000000`, exact output allocation/publication.
- `current-generated/node_records.metal` and `current-generated/metal-compile.log`:
  source lowering and Metal compilation evidence.
- `build-incremental.log` and `node-metadata/test.log`: runtime rebuild and
  owned metadata regression evidence.

This closes only bounded output allocation/publication. GPU downstream
scheduling, output-ID consumption, recursion/fan-out, cross-group sharing,
record-driven grids, and general resource/argument tables remain open.

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
