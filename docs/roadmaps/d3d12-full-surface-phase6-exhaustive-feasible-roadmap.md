# D3D12 Phase 6 exhaustive-feasible completion roadmap

**Status:** Open. The bounded Phase 6 provider matrix is closed, but Phase 6 is
not finally complete under this roadmap until every legal ordinary-graphics
combination that can be implemented on the proof host is implemented and
behavior-proven.

**Target:** Agility SDK 1.619.5, MetalSharp Wine 11.5, Apple M4/Metal 4,
Xcode 27 beta 6, pinned LLVM 15, and `METAL_SHADER_CONVERTER=/nonexistent`.

**Existing checkpoint:**
`tools/d3d12-metal-sdk/contracts/phase6-graphics-coverage.json` records the
closed 14-row bounded provider matrix. This document is the follow-up required
to replace the phrase “Phase 6 complete” with an exhaustive-feasible claim.

## 1. Completion rule

The final Phase 6 claim means:

1. Every legal **ordinary graphics** request in the expanded Phase 6 surface
   has a native Metal provider, an explicit command/replay provider, or the
   permitted CPU/GPU reference provider.
2. Every provider has exact positive behavior evidence, exact invalid-input
   evidence, source/runtime provenance, and bounded completion/readback.
3. A legal request is never rejected merely because the current implementation
   has not been written yet.
4. A request is excluded from the final feasible surface only after a recorded
   feasibility investigation proves that the Apple M4/Metal 4 host or the
   pinned toolchain cannot provide the required semantics, including after
   permitted emulation and source-owned fixtures have been considered.
5. Capability fields describe the completed matrix. A coarse D3D12 capability
   cannot be promoted if its advertised legal combinations still fail.
6. Invalid descriptors and semantically invalid combinations continue to return
   the documented validation error. They are not implementation gaps.

“Not currently implemented” is not an impossibility result. A missing Metal
primitive may be handled by a replay, shader-lowering, geometry-expansion, or
CPU/GPU reference provider. Conversely, a provider that returns `S_OK` and
silently drops work is not an acceptable fallback.

This roadmap does not pull mesh/amplification/work-graph behavior from Phase 7,
DXR behavior from Phase 8, or video/protected/DSR behavior from later phases.
Those are separate surfaces. Phase 6 does include their ordinary graphics
prerequisites where they affect a legal vertex/pixel/depth/MSAA request.

## 2. Current baseline and remaining work

### 2.1 Closed bounded evidence

The existing checkpoint proves, on the selected isolated runtime:

- graphics PSO stages and the declared fixed-function state matrix;
- raw, structured, typed-buffer, typed 2D/2D-array ROV cases, D32/D24S8
  state, and pixel-stage/independent-logic negative cases;
- six conservative-raster reference cases, including clipping, viewport,
  scissor, winding, depth, and degenerate triangles;
- the declared programmable sample-position pattern and resolve;
- two-view view-instancing/mask routing;
- default-perspective `SV_Barycentrics`;
- VRS image/rate/layout cases;
- writable MSAA, resource/view/format, topology, stencil, and dynamic-depth
  bias cases;
- exact D3D10/D3D11 legacy routing through matching selected-runtime aliases.

These rows remain regression gates. They are not a reason to reject a new
legal row below.

### 2.1.1 Implementation checkpoint (not final completion)

The first exhaustive-feasible implementation slice now has source-owned
artifacts for the following behavior, without changing the bounded manifest's
historical status:

- The authoritative equivalence-class contract is
  `contracts/phase6-exhaustive-coverage.json`, and
  `validate-phase6-exhaustive.py` rejects closed rows without exact positive
  and negative evidence or a complete no-go record.
- The disposable `run-phase6-exhaustive.sh` lane stages all PE/Unix halves
  below `/private/tmp`, verifies the Winemetal export/call-table contract, uses
  unique aliases, and removes its prefix/build outputs on normal and failure
  paths. Its source-build path relinks consumers after the Meson symbol
  extractor so the import library and staged builtin cannot silently diverge.
- P6-D's native stage-in provider has exact perspective, noperspective,
  centroid, sample, flat, explicit centroid/sample/snapped evaluation, and
  malformed-DXIL null-PSO evidence. Snapped offsets preserve D3D12's signed
  sixteenth-pixel-from-center origin when converted to Metal's normalized
  offset coordinate.
- P6-C has an exact one-pixel point/14-pixel line baseline and explicit
  `RasterizerDesc2` modes 0--3 plus invalid-mode rejection. The four-valued
  line mode is retained in the PSO/replay metadata; quadrilateral-wide/narrow
  semantic coverage remains open until a geometry/reference provider proves it.
- P6-G has exact ordered three-draw ROV increments for the D3D12 1D,
  1D-array, and 3D resource kinds using DXMT's documented height-one 2D view
  representation for 1D resources.

This is an implementation checkpoint only. The rows that still say `open`
remain blockers for the final Phase 6 claim.

### 2.2 Open legal combinations

The following are open because they have not yet received complete exact
behavior evidence. They must be implemented or moved to the proven no-go ledger
with host/toolchain evidence.

| ID | Surface | Current boundary | Required final work |
|---|---|---|---|
| P6-I | Interpolation qualifiers and evaluation | Default-perspective barycentrics only | Prove and implement all host-feasible perspective, noperspective, centroid, sample, flat/nointerpolation, and evaluation-at-sample/centroid forms in the applicable pixel paths. |
| P6-R | Ordinary point/line rasterization | WMT point/line classes exist but are not a complete exact readback matrix | Prove point and line coverage, clipping, MSAA, depth, blending, sample masks, and all supported line modes. |
| P6-R2 | `D3D12_RASTERIZER_DESC2` | `RasterizerDesc2Supported=false`; line mode is collapsed during conversion | Preserve all fields, implement or emulate each feasible line mode, and promote only after exact line-mode behavior. |
| P6-C | Conservative rasterization cross-products | Single-target, single-sample solid-triangle reference provider | Expand to all host-feasible sample counts, array layers, MRT/depth-stencil, VRS, viewport/scissor, forced-sample, sample-mask, clipping, and interpolation interactions. |
| P6-S | Programmable sample positions | One exact four-pixel/four-sample pattern | Cover every host-feasible sample count and legal position/pixel-count equivalence class, reset behavior, per-sample coverage, resolve, and invalid positions. |
| P6-V | View instancing | Exact two-view mask/array provider | Cover every host-feasible view count, mask, viewport/RT-array layout, depth, MSAA, and per-view state interaction. |
| P6-M | MSAA graphics semantics | Writable MSAA and selected resolve/resource cases | Cover ordinary render-target MSAA, sample frequency, `SV_SampleIndex`, `SV_Coverage`, sample masks, forced sample count, depth/stencil, arrays, formats, and all host-feasible counts. |
| P6-O | ROV resource/state breadth | Declared raw/structured/typed 2D matrix | Cover every host-feasible ROV resource dimension/format, MSAA ROVs, arrays, depth/stencil, barriers, blending, and ordering. |
| P6-L | Independent logic operations with side effects | UAV/ROV pixel shaders reject because per-target replay duplicates effects | Try a semantics-preserving single-pass or split-provider design; retain rejection only for combinations proven impossible to preserve exactly. |
| P6-F | Fixed-function cross-product | Selected graphics PSO cases | Add systematic depth/stencil, blend, logic, write-mask, cull/fill, depth-clip, alpha-to-coverage, forced-sample, MRT, and raster-state combinations. |
| P6-Q | Feature reports | Some fields intentionally conservative | Derive ROV, conservative raster, RasterizerDesc2, barycentric, view-instancing, sample-position, MSAA, and related fields from the final matrix rather than literals. |

The “open” label is important: it does not assert that all rows are feasible,
but it prohibits treating lack of an implementation as proof of impossibility.

## 3. Feasibility and no-go protocol

Every open row follows this order. A row cannot be closed as impossible by a
compiler error or a failed first implementation attempt.

### 3.1 Establish D3D12 legality

For each candidate request, record:

- the Agility 1.619.5 descriptor/interface definition;
- whether the combination is legal or an expected validation error;
- the exact expected HRESULT for invalid input;
- the shader stage, resource/view shape, sample count, and state that affect
  behavior.

Legal and invalid cases must not be mixed in one “unsupported” result.

### 3.2 Try providers in order

1. **Native Metal 4:** use the matching Metal primitive and preserve the
   D3D12 state directly where possible.
2. **Command/replay provider:** retain state and replay the operation with
   explicit ordering and resource lifetime. Do not use a CPU scheduler for
   shader or Work Graph execution.
3. **Shader-lowering provider:** preserve DXIL signature metadata, qualifiers,
   resource flags, sample state, and side effects through LLVM IR and MSL.
4. **Geometry/reference provider:** expand primitives or use a CPU/GPU
   reference rasterizer when Metal has no conservative-raster primitive. The
   reference must produce the same D3D12-visible pixels, coverage, depth, and
   side effects.
5. **Source-owned fixture:** if pinned DXC cannot express a legal shader (for
   example a missing ROV-MSAA HLSL template), use a checked-in DXIL/LLVM
   fixture or a source-owned assembler path before declaring a toolchain no-go.

### 3.3 Prove a genuine no-go

A no-go record must include:

- the smallest legal reproducer;
- a native Metal 4 capability/behavior probe;
- the attempted explicit provider and why it cannot preserve semantics;
- the toolchain version and, where relevant, a source-owned fixture attempt;
- the exact guarantee that cannot be met (coverage, ordering, precision,
  security, or ABI);
- the capability/reporting decision and the remaining user-visible HRESULT;
- a link to the result artifact and the source/runtime hashes.

“Metal has no direct API” is not sufficient by itself. A CPU/reference or
replay provider may still be valid. “DXC has no HLSL template” is not
sufficient by itself when the runtime can consume a valid source-owned DXIL
fixture. A no-go is allowed only when no permitted provider can reproduce the
D3D12-visible behavior on the selected host/toolchain.

## 4. Work packages and dependency order

### P6-A — Freeze the exhaustive contract and matrix

**Dependencies:** none.

**Work:**

- Add an authoritative exhaustive-feasible manifest, proposed as
  `tools/d3d12-metal-sdk/contracts/phase6-exhaustive-coverage.json`.
- Keep the existing bounded manifest unchanged as a historical/regression
  contract; do not silently relabel its 14 rows as exhaustive.
- Enumerate dimensions rather than relying on a few hand-picked PSOs:
  primitive/topology, line mode, shader stage, interpolation mode, viewport
  and scissor, culling/winding, depth/stencil, blend/logic/write mask, MRT,
  sample count/positions, MSAA array/resolve, VRS, view instance, ROV resource,
  and invalid descriptors.
- Give every row a stable ID, legality, provider, positive probe, negative
  probe, exact expected output, provenance, and status of `open`, `closed`, or
  `proven_no_go`.
- Add a validator that refuses `closed` when a legal row has no exact positive
  and negative evidence.

**Exit evidence:** the manifest has no unclassified legal combination in its
chosen equivalence classes, and the validator can distinguish invalid input,
open work, and proven no-go.

### P6-B — Build the host/toolchain capability inventory

**Dependencies:** P6-A.

**Work:** add small source-owned Metal probes for:

- fragment barycentric and interpolation builtins, including perspective and
  no-perspective forms;
- `setSamplePositions`, legal sample counts, per-pixel positions, sample
  coverage, and resolve behavior;
- MSAA color/depth/stencil arrays and sample-frequency inputs;
- point/line rasterization, antialiased lines, and line width/coverage modes;
- viewport arrays, render-target arrays, view masks, and VRS interactions;
- raster-order groups for every candidate texture/buffer shape;
- global logic-op behavior and any safe multi-target alternative.

The inventory is not feature promotion. It identifies which providers must be
written and which host claims require a no-go investigation.

**Exit evidence:** every P6 row has a selected provider strategy or a pending
no-go experiment, with no row rejected solely from an untested assumption.

### P6-C — Complete ordinary point/line/rasterizer-state behavior

**Dependencies:** P6-B; reopen Phase 5 signature/lowering code only where a
shader input/output prerequisite is missing.

**Work:**

- Add exact point-list and line-list/strip draws with clipping, viewport and
  scissor offsets, depth, culling, blend, write masks, sample masks, and MRT.
- Preserve the full `D3D12_RASTERIZER_DESC2` fields through pipeline-stream
  parsing, PSO storage, cache keys, and replay. Do not collapse
  `LineRasterizationMode` to a boolean antialias flag.
- Test `ALIASED`, `ALPHA_ANTIALIASED`, `QUADRILATERAL_WIDE`, and
  `QUADRILATERAL_NARROW` modes. If Metal cannot express a mode directly,
  evaluate a validated line-to-quad/coverage provider before considering a
  no-go.
- Add exact line/point MSAA and depth readbacks, including endpoint,
  horizontal, vertical, diagonal, clipped, and zero-length cases.
- Implement `RasterizerDesc2Supported` only when all advertised modes and
  invalid descriptors have exact evidence. Until then it must remain false.

**Exit evidence:** every supported ordinary primitive/mode has pixel/coverage
readback and malformed/unsupported descriptors return the documented error;
there is no silent point/line drop.

### P6-D — Finish interpolation and barycentric semantics

**Dependencies:** P6-B and P6-C.

**Work:**

- Audit PSV0/input-signature parsing, DXIL IR metadata, MSL attribute emission,
  and fragment ABI mapping in:
  `vendor/dxmt/src/airconv/dxil/dxil_ir.cpp`,
  `dxil_to_msl.cpp`, `msl_lowering.cpp`, and the pipeline signature path.
- Add fixtures for perspective, `noperspective`, `centroid`, `sample`,
  `nointerpolation`, and the relevant `SV_Barycentrics` forms.
- Test values at center, edge-only, centroid, and every selected sample
  location. Use analytic barycentric values and bit-exact integer readback,
  not visual similarity.
- Cover `SV_SampleIndex`, `SV_Coverage`, helper/partial-coverage lanes,
  `EvaluateAttributeAtSample`, `EvaluateAttributeAtCentroid`, and any legal
  DXIL evaluation form that is exposed by the target shader model.
- Cross-check interpolation with perspective depth, viewport offsets, MSAA,
  conservative rasterization, and view instancing.
- Keep `AttributeAtVertex`'s existing bounded provider separate; expand it only
  with its own exact signature/type/topology evidence.

**Exit evidence:** every host-feasible interpolation/evaluation form has exact
fragment readback at known sample locations and exact invalid-signature
rejection. `BarycentricsSupported` is derived from the complete advertised
form set, not only the default-perspective fixture.

### P6-E — Expand MSAA and programmable sample positions

**Dependencies:** P6-B and P6-C; resource/layout prerequisites may reuse the
Phase 3 provider but must not be assumed from resource creation alone.

**Work:**

- Test ordinary render-target and depth/stencil MSAA for each host-feasible
  sample count, including 1, 2, 4, 8, 16, and 32 where the host accepts them.
  A host rejection must be captured as a capability result, not hidden by
  fallback.
- Expand `SetSamplePositions` to legal sample-count and pixel-count classes,
  coordinate boundaries, reset/default behavior, duplicate positions, and
  per-pixel position selection.
- Exercise sample coverage, sample index, sample mask, forced sample count,
  per-sample shading, array slices, depth/stencil, and resolve modes.
- Add exact raw-sample readback before resolve and exact resolved readback
  after resolve. Validate row pitch, slice, and array-layer identity.
- Validate all affected command records in
  `d3d12_command_list.cpp`/`d3d12_command_queue.cpp`, including close/reset/
  reuse and state changes between draws.

**Exit evidence:** each advertised count/pattern/state has exact per-sample
and resolved output; every unsupported host count has a documented capability
result and pre-execution failure rather than a wrong fallback.

### P6-F — Expand conservative rasterization

**Dependencies:** P6-C and P6-E; P6-D for interpolation/coverage outputs.

**Work:**

- Keep the CPU/GPU reference provider as the baseline; a native Metal
  conservative-raster primitive is not required if the reference preserves
  D3D12-visible semantics.
- Extend the reference model and replay payload beyond the current
  single-target, single-sample shape: sample coverage, array layers, MRT,
  depth/stencil, VRS, forced sample count, sample masks, alpha/blend state,
  viewport/scissor, clipping, winding, top-left rules, subpixel boundaries,
  and all legal triangle topologies.
- Validate `SV_InnerCoverage` for fully covered and edge-only pixels/samples.
- Verify degenerate triangles against the documented D3D12 rule, including
  clipped and depth-enabled variants. Do not infer behavior from a single
  zero-area case.
- Treat conservative line requests according to the D3D12 legality rules;
  ordinary line behavior belongs to P6-C and RasterizerDesc2 behavior belongs
  to P6-C/P6-G.
- Add property-based/reference cases around every pixel and sample boundary,
  then retain a small deterministic corpus for the runtime gate.

**Exit evidence:** provider and reference agree per pixel and per sample for
all advertised cross-products; no conservative-raster combination is rejected
because the current reference payload happens to be narrow.

### P6-G — Complete ROV resources and side-effect-safe logic operations

**Dependencies:** P6-C, P6-D, and P6-E.

**Work:**

- Expand ROV fixtures and lowering for every host-feasible pixel resource
  dimension and format: buffer, byte-address, structured, typed 1D/2D/3D,
  arrays, and MSAA forms where legal.
- When pinned DXC lacks an HLSL declaration, generate a valid source-owned
  DXIL fixture or use the checked-in LLVM/DXIL assembly path. Do not close the
  row as impossible merely because `RasterizerOrderedTexture2DMS` is missing
  from the HLSL headers.
- Test ordering with overlapping primitives, depth/stencil pass/fail,
  sample frequency, arrays, barriers, counters, and all relevant view flags.
- Replace the current independent-logic/UAV rejection only if a provider can
  guarantee each shader side effect occurs exactly once in D3D12 order. Explore
  a single-pass target strategy, side-effect/color pass separation with
  ordering proof, or shader-side effect extraction. A repeated draw without
  such a proof is forbidden.
- Keep vertex/compute ROV rejection when it is the documented D3D12 stage rule;
  it is not an implementation gap.

**Exit evidence:** exact ordered values, one-shot side-effect counters, depth/
stencil interactions, and invalid-stage/resource rejection are all recorded.
`ROVsSupported` is promoted only for the complete advertised matrix.

### P6-H — Reconcile capabilities and negative behavior

**Dependencies:** P6-C through P6-G.

**Work:**

- Replace literal capability promotion with predicates over the exhaustive
  manifest and provider evidence.
- Reconcile at least:
  `ROVsSupported`, `ConservativeRasterizationTier`,
  `RasterizerDesc2Supported`, `NarrowQuadrilateralLinesSupported`,
  `BarycentricsSupported`, `ViewInstancingTier`,
  `ProgrammableSamplePositionsTier`, writable-MSAA fields, and related
  Options 14/15/16/19 values.
- For every capability, test both a positive query and a legal request at the
  edge of the advertised range. Test invalid enum, count, format, view, and
  descriptor cases separately.
- Ensure failed public PSO creation returns `E_FAIL` with a null object only
  for a genuinely unsupported/no-go request or invalid descriptor, and never
  returns a usable-looking object whose draw is dropped.
- Add diagnostic reasons that identify the provider boundary and the exact
  unsupported shape.

**Exit evidence:** capability reports and runtime acceptance/rejection agree;
there is no field that advertises a broader surface than the final manifest.

### P6-I — Add one authoritative exhaustive gate and close Phase 6

**Dependencies:** P6-A through P6-H.

**Work:**

- Add a dedicated phase-6 runner, preferably
  `tools/d3d12-metal-sdk/scripts/run-phase6-exhaustive.sh`, rather than
  inferring Phase 6 success from the broader full-surface runner whose later
  Phase 7–16 failures are unrelated.
- The runner must build source-owned probes, stage matching PE/Unix Winemetal
  artifacts, create a disposable prefix, select the pinned runtime, preserve
  `METAL_SHADER_CONVERTER=/nonexistent`, run every positive/negative Phase 6
  group, and remove the prefix on every exit path.
- Add `validate-phase6-exhaustive.py` to require exact result fields, bounded
  waits, non-null/null object invariants, readback arrays, HRESULTs, provider
  names, and runtime/source hashes.
- Run the source build, ABI/export check, runtime layout/provenance check,
  contract validators, phase-0 inventory validator, probe-matrix validator,
  `git diff --check`, and the dedicated fresh-runtime gate.
- Re-run the existing bounded probes unchanged as regression tests.
- Update `phase6-graphics-coverage.json` only after the exhaustive manifest is
  closed; retain the bounded checkpoint history in the proof document.

**Exit evidence:** zero open legal-feasible rows, zero unclassified rejections,
zero silent-success/no-op paths in the Phase 6 surface, exact fresh-prefix
readbacks for every advertised provider, and a clean source-only commit.

## 5. Required probe matrix

The final gate must include, at minimum:

1. Point and line primitive coverage, all feasible RasterizerDesc2 line modes,
   endpoint/clip/zero-length cases, and invalid descriptors.
2. Perspective, noperspective, centroid, sample, flat/nointerpolation,
   barycentric, sample-evaluation, helper-lane, and partial-coverage outputs.
3. MSAA color/depth/stencil at every accepted sample count, raw sample values,
   coverage/index/mask, forced sample count, array slices, and resolve.
4. Programmable sample positions across legal count/pixel/reset/boundary
   equivalence classes.
5. Conservative triangle coverage at subpixel/top-left/degenerate/clipping
   boundaries and all advertised MSAA/VRS/array/depth/MRT interactions.
6. View-instancing count/mask/layout/depth/MSAA combinations.
7. ROV buffer/texture dimension/format/MSAA/depth/stencil/order matrices,
   including one-shot side-effect checks.
8. Independent logic-op state for every provider-safe shader class, plus exact
   rejection and diagnostics for any remaining side-effect-unsafe class.
9. Fixed-function depth/stencil/blend/logic/write-mask/cull/fill/clip/
   alpha-to-coverage/forced-sample/MRT combinations selected by equivalence
   classes and exact expected outputs.
10. Null, malformed, out-of-range, unsupported-enum, mismatched-format,
    mismatched-count, and unavailable-resource validation cases.

A PSO-creation result alone is never sufficient. Each positive row must execute
and read back pixels, samples, coverage, depth/stencil, or an ordered side effect.
Each negative row must check HRESULT and output-object nullness.

## 6. Feasibility exclusions that may be accepted

The following are **not** automatically excluded; they require the no-go
protocol:

- Metal's lack of a direct conservative-raster API;
- Metal's global rather than per-render-target logic-op state;
- missing pinned-DXC syntax for an ROV resource;
- missing native line-rasterization mode or line-width API;
- host sample-count limits above the M4's accepted MSAA range;
- unsupported Metal interpolation qualifiers or sample builtins.

A final `proven_no_go` entry is acceptable only when a reference/replay/shader
provider cannot preserve the D3D12-visible semantics on this host. The final
report must name the exact limitation and leave the corresponding capability
unadvertised. It must not call the entire Phase 6 surface complete while
silently folding a feasible row into that exclusion.

ROV vertex/compute requests, malformed descriptors, invalid sample counts,
invalid line/conservative combinations, and other documented validation
failures remain expected negative cases and are not “feasible rows.”

## 7. Cross-phase dependencies

- **Phase 3:** supplies resource allocation, views, formats, arrays, and MSAA
  backing. Phase 6 must still prove rendering semantics; successful resource
  creation is not rendering evidence.
- **Phase 4:** supplies command recording/replay, sample positions, view masks,
  state transitions, and readback ordering. Any new state record must be added
  to the Phase 4 command inventory as well as the Phase 6 behavior gate.
- **Phase 5:** supplies DXIL parsing, interpolation metadata, shader resource
  flags, and MSL lowering. New shader semantics belong in the shader corpus,
  but their graphics readback belongs here.
- **Phase 7:** owns mesh/amplification/work-graph breadth and mesh-specific
  derivatives/per-primitive VRS. Those do not block ordinary Phase 6 closure.
- **Phase 15:** performs exhaustive differential/game validation after this
  provider gate; it cannot be used to excuse an open Phase 6 provider row.

## 8. Final completion checklist

- [ ] Exhaustive-feasible manifest added and validated.
- [ ] Every legal Phase 6 equivalence class has a provider or a proven no-go.
- [ ] Point/line and RasterizerDesc2 behavior is exact or has a documented
      host-feasibility blocker.
- [ ] All host-feasible interpolation qualifiers and sample evaluation forms
      are exact.
- [ ] All host-feasible MSAA/sample-position/resolve interactions are exact.
- [ ] Conservative-raster cross-products are exact; no narrow reference shape
      is being advertised as a broad tier.
- [ ] ROV resource/state breadth and side-effect ordering are exact.
- [ ] Logic-op replay never duplicates UAV/ROV/depth/stencil side effects.
- [ ] Feature fields are derived from evidence and match runtime behavior.
- [ ] Invalid requests have exact HRESULT/null-object evidence.
- [ ] Dedicated fresh-prefix Phase 6 gate passes with bounded waits and cleanup.
- [ ] Source build, ABI/provenance, contract, inventory, and matrix validators
      pass.
- [ ] No binaries, caches, prefixes, logs, or generated runtime artifacts are
      committed.
- [ ] Only then is the final Phase 6 status changed from bounded closure to
      exhaustive-feasible completion.
