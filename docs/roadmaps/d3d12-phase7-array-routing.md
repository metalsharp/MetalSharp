# Phase 7 output-array routing

Status: **bounded D3D12 dispatch verified; Phase 7 remains incomplete**.
User-selected architecture: per-program GPU routing tables, not packed array-index bits.

## Implemented

- DXIL metadata retains finite/unbounded array sizes, sparse flags, and declared
  NodeID names/indices. Full-width metadata representation does not remove the
  specification's graph/node limits.
- Creation builds an immutable per-program table from actual graph nodes, never
  by enumerating an unbounded declared size. Dense ranges must be populated;
  missing targets require AllowSparseNodes. Duplicate identities reject.
  The builder also checks the nonrecursive minimum node budget by summing each
  named array's span, including holes. Host tests cover the exact limit, one-over
  rejection, same-name entries, and declaration-order independence. Recursion's
  additional budget charges remain part of the unfinished recursion validator.
- Canonical and entrypoint shader copies share the same snapshot through the
  registry. GPU uploads cache by snapshot identity and retain buffers through
  submission completion, including cache reuse.
- Sorted 24-byte rows use `(source_node, metadata_index, array_index)` keys.
  Tokens are row index plus one. Each declaration retains an element-zero row
  and an invalid row; sparse misses cannot alias a populated element zero.
- GPU lookup implements handle creation, dynamic indexing, and destination-based
  IsValid. Routing-aware lowering retains legacy-context compatibility and
  rejects invalid routed contexts rather than falling back to scalar IDs.
- Version-4 raw and version-5 descriptor contexts preserve the 40-byte prefix,
  adding GPU table address, count and source node in a 56-byte layout. Scheduler
  and ICB context generation propagate these fields. Direct/ICB encoders declare
  table read usage without consuming another user-visible register.
- Allocation records publish route tokens. Each token has one destination, so
  routed consumption uses one bit rather than metadata-index bit packing.

## Evidence

`/private/tmp/wg-output-arrays/dispatch-final/` contains 41 passing official
Work Graph/bridge results; every required Work Graph contract assertion passes.
Metal API Validation reports no failed assertion. This is dirty development
staging, **not** clean-release or independent rebuild provenance.

The D3D12 creation probe accepts the dense/sparse graph and rejects removal of a
required dense target with E_FAIL and a null state object. Dispatch checks four
dense elements and sparse index 65536, yielding exactly
`[101,202,303,404,505,0,0,0,0,0,0,0,0,0,0,0]`. Sparse misses produce no consumer
writes. CPU records are mutated after recording, before execution, without
changing the result. JSON includes exact readback and all output words.

Separate evidence remains distinct:
- Host metadata/table tests cover array sizes, NodeID, duplicates and missing targets.
- The native Metal helper probe checks eight GPU lookups with GPU-produced indices,
  versions 4/5, sparse misses and invalid contexts/handles. Its JSON correctly
  remains `d3d12_integrated=false`; D3D12 integration has its own probe.
- Both array shader entries compile with `-std=metal4.0`. A lowering regression
  checks actual routing calls survive expression coercion rather than becoming zero.
- Existing ICB tests pass, but they do not certify routed-array ICB consumers.

## Remaining

- GPU input and GPU-producer dependencies for the routed array path.
- Array consumers using record-driven ICB broadcasting; routed fan-out and repeated
  dispatch/state-object lifecycle breadth beyond the current fixture.
- Empty-output arrays, output-limit/overflow semantics, recursion, node overrides,
  complete graph-limit accounting and general resource/local-root arguments.
- Additional malformed metadata and array-layout validation, broad regressions,
  synchronized clean staging, strict ABI audit and reproducibility evidence.
- All other Phase 7 exit requirements and later roadmap phases.

Reference: [DirectX Work Graphs specification](https://microsoft.github.io/DirectX-Specs/d3d/WorkGraphs.html),
node limits, output attributes, metadata and IsValid sections.
