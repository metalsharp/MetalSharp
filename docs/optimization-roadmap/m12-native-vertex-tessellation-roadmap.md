# M12 Native Vertex/Tessellation GPU Roadmap
**Updated:** 2026-08-05

> **Status note (2026-08-05):** M12's default backend is now **vkd3d-proton**
> (D3D12 → Vulkan → VKMT MoltenVK → Metal, PR #377). The DXMT-internal
> vertex/tessellation path this roadmap describes ("inside DXMT/M12") now
> applies only to the `m12Backend=dxmt` **rollback** lane — the default
> vkd3d-proton route has no in-house IA/tessellation code (vkd3d-proton + DXVK
> handle it). Treat all `vendor/dxmt/...` implementation points below as
> rollback-lane work unless the roadmap is explicitly re-scoped to the
> vkd3d-proton stack.

Status: final source-backed roadmap for PR #230 follow-up work
Branch/worktree: `feat/m12-fresh-proof-game-harness` at `/Volumes/AverySSD/MetalSharp-SM6-UE-Lab/10-worktrees/metalsharp-m12-fresh-proof-pr`
Primary post-crash evidence root: `/tmp/pr230-elden-postcrash-persistent-evidence-20260629-002331`

## Mission

Build **M12 Native Vertex/Tessellation Path** (`M12-NVTP`): a D3D12-specific GPU path inside DXMT/M12 that resolves input-assembler, vertex-buffer, index-buffer, patch/tessellation, compute, resource-lifetime, and command-buffer semantics explicitly before any future commercial game launch.

This roadmap is intentionally **anti-fallback**:

- Existing `D3D12 tessellation fallback` is a defect signal, not a success path.
- Existing `M12 skipping unsafe DrawIndexedInstanced reason=vertex_range_oob` is a defect signal, not a visual workaround.
- CPU-only geometry/tessellation fallback is forbidden for correctness acceptance. It can create the same class of backpressure/resource lifetime failures that this roadmap is designed to remove, and the project is treating the DXMT maintainer warning about CPU fallback pressure as a hard design constraint.
- Unsupported native GPU shapes must fail closed with a named diagnostic and artifact, not render through a degraded path.

Working implementation names:

- `d3d12_native_vertex_path.*` — D3D12 IA/VB/IB/draw-state resolution, validation, Metal binding, and draw breadcrumbs.
- `d3d12_native_tessellation_path.*` — D3D12 HS/DS patch topology, tessellation-factor generation, and Metal patch draw encoding.
- `m12_native_vertex_tessellation` — shared counters, proof gates, logs, and mini-game/probe outputs.

D3D12-native ownership rule:

- Phase 5 must not port, subclass, or depend on the existing D3D11 tessellation context/pipeline/shader machinery. D3D11-oriented code may be inspected only as historical context for low-level Metal/WMT capability, not reused as the D3D12 implementation basis.
- Shared low-level bridge primitives are allowed only when they are API-neutral (`winemetal` C structs/thunks, Metal feature queries, resource wrappers). Any D3D12 tessellation state machine, PSO metadata, root binding, draw validation, and proof diagnostics must live in D3D12-owned code paths.

## Absolute gates before another Elden Ring launch

No live Elden launch is allowed until all local gates below pass under the PR runtime/backend:

- `vertex_range_oob=0` for all valid `m12_fresh_game.exe` proof lanes.
- `D3D12 tessellation fallback=0` in all proof and prelaunch logs.
- `M12 compute encoder encode failed=0` under compute stress.
- `MTLCommandBufferErrorDomain=0` under diagnostic command-buffer stress.
- Bounded in-flight command buffers and drawable acquisition; no unbounded `nextDrawable`/WindowServer stall.
- Exact Elden shader replay remains clean and active `.msl.err.txt` freshness checks remain zero.
- `/mtsp/prepare` proves game-local DLL hashes match the PR runtime.
- User gives explicit approval for any commercial launch.

## Failed Elden run evidence and what each failure indicates

Evidence files:

- Final launch log copy: `/tmp/pr230-elden-postcrash-persistent-evidence-20260629-002331/logs/launch-1782713377.log`
- Final run inventory: `/tmp/pr230-elden-postcrash-persistent-evidence-20260629-002331/analysis/final-run-error-inventory.txt`
- Shader cache summary: `/tmp/pr230-elden-postcrash-persistent-evidence-20260629-002331/shader-cache/shader-cache-summary.json`
- System evidence rollup: `/tmp/pr230-elden-postcrash-persistent-evidence-20260629-002331/EVIDENCE_ROLLUP.json`

Observed user-facing symptoms:

- Elden progressed farther than before: loading icon appeared and the user got past character creation.
- The 3D character model still failed to initialize/render correctly; it appeared missing/red/solid-color.
- Runtime slowed progressively.
- User observed audio glitches during slowdown.
- The machine eventually crashed/rebooted.

Active final-run runtime issues:

| Issue | Evidence | What it indicates |
|---|---:|---|
| Tessellation path is not native | `238` warnings shaped as `D3D12 tessellation fallback: compiling VS/PS-only render PSO ... topology=4` | HS/DS shaders and patch topology are reaching M12, but the current path compiles a VS/PS-only render PSO. Character/body geometry can plausibly fail when patch/tessellated geometry is discarded, flattened, or approximated incorrectly. |
| Indexed draws skipped for vertex OOB | `256` warnings shaped as `M12 skipping unsafe DrawIndexedInstanced reason=vertex_range_oob` | Current IA/VB/IB range resolution believes draw parameters would read beyond bound vertex buffers. This directly matches missing geometry. |
| Compute encode failure | `1` line: `M12 compute encoder encode failed label=Dispatch pso=0x5312eaa0 dispatch=1x1x16` | Likely invalid resource/argument binding, poisoned encoder state, incomplete compute root/descriptor binding, or command-buffer resource lifetime/residency issue. |
| Present path alive | `M12 present ... classification=drawn`, final present count around `960` | This was not a pure no-present failure; the renderer stayed alive while geometry/command state degraded. |
| Shader compile path clean in final epoch | `shader_compile_fail=0`, `pso_compile_failed=0`, `CreateGraphicsPipelineState_failed=0`, `post_final_launch_msl_err_count=0` | Latest failure must be investigated as runtime draw/resource/command correctness, not only shader syntax compilation. |
| Historical/stale MSL sidecars still present | `total_msl_err_count=15`, `post_final_launch_msl_err_count=0` | Prior shader bugs were real and fixed/replaced by newer `.msl`; sidecars must stay tracked as historical hazards but not confused with active final-run errors. |
| WindowServer/system stall and reboot | WindowServer ping timeouts before reboot; `kern.boottime Mon Jun 29 00:15:02`; watchdog/panic-flow boot messages | The run destabilized the graphics/session environment. Do not overclaim a direct preserved AGX panic, but treat watchdog/reboot evidence as a hard launch-safety blocker. |

Representative `vertex_range_oob` line:

```text
warn:  M12 skipping unsafe DrawIndexedInstanced reason=vertex_range_oob pso=0x635227d0 vs=0a9d2ab3d81f9b7d ps=cfee6b49d62139d5 ... gpu=0x10063560000 size=10944 stride=12 required=6708 available=912 elems=1716 inst=1 start=4992 base=0 start_inst=0 indexed=1
```

Representative tessellation warning:

```text
warn:  D3D12 tessellation fallback: compiling VS/PS-only render PSO HS bytes=12796 DS bytes=24286 topology=4
```

Historical MSL sidecar classes found in the Elden shader cache:

- `9x` `float2` constructor errors.
- `5x` `int4` constructor over-arity errors.
- `1x` bool vector/scalar mismatch.

These shader hazards remain regression gates, but they are not the active final-run blocker once freshness proves final-run MSL compilation was clean.

## Source-backed technical constraints

### D3D12 IA and draw semantics that M12-NVTP must preserve

- `IASetVertexBuffers(StartSlot, NumViews, pViews)` binds vertex-buffer views starting at a zero-based slot. The native path must preserve D3D12 input slots instead of compacting them into an ambiguous runtime slot order.
- `D3D12_VERTEX_BUFFER_VIEW` consists of `BufferLocation`, `SizeInBytes`, and `StrideInBytes`; stride is part of draw-time IA state.
- `D3D12_INDEX_BUFFER_VIEW` consists of `BufferLocation`, `SizeInBytes`, and `Format`; validation must use the view size, not the full backing resource size.
- `D3D12_INPUT_ELEMENT_DESC` carries `SemanticName`, `SemanticIndex`, `Format`, `InputSlot`, `AlignedByteOffset`, `InputSlotClass`, and `InstanceDataStepRate`. Range validation must include element offsets and DXGI format byte widths.
- `D3D12_APPEND_ALIGNED_ELEMENT` must be resolved using D3D packing rules when building the input layout.
- `DrawIndexedInstanced` is a two-stage lookup: read indices beginning at `StartIndexLocation`, then add signed `BaseVertexLocation` before resolving vertex rows.
- `StartIndexLocation` must be applied exactly once. If Metal index-buffer offset already accounts for it, shader helper state must not add it again.
- Per-instance input validation must use `StartInstanceLocation`, `InstanceCount`, and `InstanceDataStepRate`, not vertex count.

### D3D12 resource-state constraints

- `D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER` is required when a subresource is accessed as a vertex or constant buffer.
- `D3D12_RESOURCE_STATE_INDEX_BUFFER` is required when accessed by the 3D pipeline as an index buffer.
- `D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT` is required for ExecuteIndirect argument buffers.
- UAV writes feeding draw/dispatch arguments or vertex/index data need explicit barrier ordering before later reads.
- M12 should keep a lightweight state ledger for buffers used as VB, IB, indirect arguments, root descriptors, and argument buffers. It does not need to become the Windows debug layer to start, but the proof harness must catch obvious missing transitions.

### Metal draw, tessellation, and lifetime constraints

- Metal render encoders expose explicit `setVertexBuffer:offset:atIndex:` and indexed draw APIs with `indexBufferOffset`, `baseVertex`, and `baseInstance`.
- Metal `drawIndexedPrimitives(... indexBufferOffset ... baseVertex ... baseInstance ...)` accepts a signed `baseVertex`; ordinary D3D12 `BaseVertexLocation` should map without a CPU workaround.
- Metal `indexBufferOffset` must be a byte offset and a multiple of the index size.
- Metal render pipelines include tessellation properties: `tessellationPartitionMode`, `maxTessellationFactor`, `tessellationFactorFormat`, `tessellationControlPointIndexType`, `tessellationFactorStepFunction`, and `tessellationOutputWindingOrder`.
- Metal render encoders expose `setTessellationFactorBuffer`, `setTessellationFactorScale`, `drawPatches`, and `drawIndexedPatches`; this is the native GPU target for patch proof, not VS/PS fallback.
- Metal `useResource`/`useResources` calls do not retain resources. M12 must retain every referenced MTL object until command-buffer completion.
- Metal heap/argument-buffer usage calls do not solve all hazards; fences are still required where data hazards exist.
- Metal command-buffer errors include timeout, page fault, invalid resource, out of memory, access revoked, and stack overflow. Page fault documentation includes out-of-boundary access as a possible cause; invalid resource includes resources deleted before command-buffer execution.
- `CAMetalLayer.nextDrawable` may return nil after a one-second timeout when all drawables are in use, while disabling drawable timeout can block forever. Proof and launch paths must be bounded.

### Ecosystem lessons from SearXNG-backed research

- MoltenVK issue searches show real Metal argument-buffer, validation, descriptor-pool lifetime, imported-texture, and device-loss-like regressions. M12 must not hand-wave descriptor/resource lifetime.
- wgpu issue searches show real Metal vertex-pulling and vertex-buffer OOB concerns. M12 must validate or robustly handle vertex/index bounds instead of relying on backend undefined behavior.
- GPUWeb discussions identify a portability mismatch: D3D12 supplies vertex stride through IA buffer views, while Metal/Vulkan often couple vertex layout/stride more tightly to pipeline layout. M12 must either use raw-slot vertex pulling or include effective D3D12 stride/layout in pipeline keys.

## Existing code entry points

Runtime implementation points:

- IA command recording: `vendor/dxmt/src/d3d12/d3d12_command_list.cpp`
- IA/draw command definitions: `vendor/dxmt/src/d3d12/d3d12_command_defs.hpp`
- Existing vertex safety helpers: `vendor/dxmt/src/d3d12/d3d12_vertex_input.hpp`
- Draw replay and compute replay: `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp`
- Tessellation fallback PSO behavior: `vendor/dxmt/src/d3d12/d3d12_pipeline_state.cpp`
- Resource and fence implementation: `vendor/dxmt/src/d3d12/d3d12_resource.cpp`, `d3d12_fence.cpp`, `d3d12_command_allocator.cpp`
- Metal bridge/lifetime calls: `vendor/dxmt/src/winemetal/unix/winemetal_unix.c`, `vendor/dxmt/src/winemetal/Metal.hpp`

Proof vehicle:

- Mini-game source: `tools/d3d12-metal-sdk/probes/m12_fresh_game/m12_fresh_game.cpp`
- Build script: `tools/d3d12-metal-sdk/scripts/build-probes.sh`
- Full proof harness: `tools/d3d12-metal-sdk/scripts/run-m12-fresh-proof.py`
- Built mini-game executable: `tools/d3d12-metal-sdk/out/bin/m12_fresh_game.exe`

The roadmap requires loading the mini-game through the PR runtime using the proof harness or equivalent Wine invocation, not through a packaged backend or unrelated runtime.

Canonical proof command shape:

```bash
python3 tools/d3d12-metal-sdk/scripts/run-m12-fresh-proof.py \
  --repo /Volumes/AverySSD/MetalSharp-SM6-UE-Lab/10-worktrees/metalsharp-m12-fresh-proof-pr \
  --visible-frames 600
```

For targeted phase work, the same executable may be launched directly by the harness after build/stage, but every direct invocation must preserve PR runtime identity, produce JSON/stdout/stderr artifacts, and scan logs for the hard fail strings.

## Phase roadmap

Each phase has three required outputs:

1. **Real code** in the PR worktree.
2. **Mini-game/probe proof** that loads `m12_fresh_game.exe` or a focused SDK probe through the PR runtime.
3. **Validity artifact** containing stdout JSON, stderr/log scans, readback/visual proof when applicable, and exact pass/fail counters.

### Phase 0 — Evidence lock and no-fallback gate wiring

Goal:

Make fallback and runtime hazard strings hard failures before changing rendering behavior.

Code changes:

- Extend `run-m12-fresh-proof.py` summary generation to count:
  - `vertex_range_oob`
  - `D3D12 tessellation fallback`
  - `M12 compute encoder encode failed`
  - `MTLCommandBufferErrorDomain`
  - active `.msl.err.txt` sidecars newer than their paired `.msl`
  - WindowServer/watchdog/reboot evidence in bounded proof windows
- Add a `no_fallback_pass` boolean and `native_runtime_pass` boolean to proof summaries.
- Ensure proof scripts fail if fallback or draw-skip counters are nonzero for valid lanes.

Mini-game proof:

- Build and load current `m12_fresh_game.exe` for a short smoke run.
- Confirm the proof summary reports all hard-fail counters, even if current code still fails some of them.

Acceptance:

- The harness can fail closed with a single summary naming the exact failure class.
- No fallback path can produce a green proof.

### Phase 1 — Native IA truth-table lanes in `m12_fresh_game.exe`

Goal:

Create deterministic local D3D12 scenes that reproduce every suspected `vertex_range_oob` cause without needing Elden.

Code changes:

- Add new mini-game lanes for:
  - R16 indexed draw with nonzero `StartIndexLocation`.
  - R32 indexed draw with the same geometry and offset scaling proof.
  - Positive `BaseVertexLocation` valid draw.
  - Negative `BaseVertexLocation` valid draw.
  - Negative `BaseVertexLocation` underflow invalid draw.
  - IBV subrange inside a larger resource.
  - VBV subrange inside a larger resource.
  - Multi-slot vertex input with different strides and nonzero `AlignedByteOffset`.
  - `D3D12_APPEND_ALIGNED_ELEMENT` layout.
  - Per-instance input with `InstanceDataStepRate > 1` and nonzero `StartInstanceLocation`.
  - Dynamic `IASetVertexBuffers` stride changes across draws using the same PSO.
  - Progressive RGB vertex triangle lane: genuinely draw the triangle out over time, starting from a single lit pixel/point-sized primitive and growing frame-by-frame through partial coverage until the final frame presents the complete RGB interpolated triangle. This must prove ordinary vertex interpolation and frame progression rather than only static square stamps or an already-complete triangle on frame 1.
  - Descriptor/root-table mutation between draws.
  - ExecuteIndirect DrawIndexed with nonzero start/base values.
- Emit one JSON block per lane with expected color/readback hash and draw parameters.

Mini-game proof:

- Load `m12_fresh_game.exe` for at least one visible/readback frame per lane.
- Read back stamped pixels and/or structured readback buffers proving the intended geometry appeared.
- For the RGB vertex triangle lane, prove progressive reveal across a bounded frame sequence: frame 0/1 has only the seed pixel/point, intermediate frames have monotonically increasing triangle coverage/sample counts, and the final frame matches the full triangle. A single static square, static flat-color triangle, or complete triangle presented immediately on the first frame is not sufficient.
- Run invalid lanes and prove they produce named diagnostics without poisoning the next valid lane.

Validity artifacts:

- Per lane: `lane_name`, `expected_rgba`, `actual_rgba`, `pass`, draw args, VBV/IBV details.
- Log scan: no valid lane may emit `vertex_range_oob`.
- Negative lanes must emit `native_vertex_blocked_expected=true`, not crash or continue silently.

Acceptance:

- Every suspected IA/index failure mode has a local proof case.
- Valid lanes render/read back correctly.
- The RGB vertex triangle lane shows a real frame-by-frame rasterization/reveal sequence: seed pixel/point → partial triangle → complete interpolated RGB triangle, matching a real-world render-loop signal instead of a one-shot static stamp.
- Invalid lanes fail closed and subsequent lanes still pass.

### Phase 2 — Implement `d3d12_native_vertex_path.*`

Goal:

Replace scattered draw safety decisions with one D3D12-specific resolver used by direct and indirect indexed draws.

Code changes:

- Add `D3D12NativeVertexPathState` containing:
  - current `D3D12_VERTEX_BUFFER_VIEW[32]`
  - current `D3D12_INDEX_BUFFER_VIEW`
  - primitive topology
  - PSO input-layout metadata
  - command-list epoch
  - root-signature epoch
  - descriptor-heap epoch
  - resource-state epoch
- Add `ResolveNativeIndexedDraw()` returning:
  - resolved IB resource and byte offset
  - index type and index size
  - sampled min/max index when readable
  - final min/max vertex ID after signed `BaseVertexLocation`
  - resolved per-slot VB resource, view offset, size, stride
  - per-input required byte ranges including `AlignedByteOffset` and DXGI element width
  - per-instance required ranges
- Validate:
  - `StartIndexLocation * indexSize + IndexCountPerInstance * indexSize <= IBV.SizeInBytes`
  - valid index format: `DXGI_FORMAT_R16_UINT` or `DXGI_FORMAT_R32_UINT`
  - signed base-vertex underflow/overflow
  - VBV view bounds, not whole-resource bounds
  - per-instance range using `StartInstanceLocation`, `InstanceCount`, and `InstanceDataStepRate`
  - GPUVA-to-resource mapping for suballocated views
- Route ExecuteIndirect draw replay through the same resolver.

Mini-game proof:

- Load `m12_fresh_game.exe` and run all Phase 1 lanes through the new resolver.
- Compare resolver output against expected lane metadata.

Validity artifacts:

- `native_vertex_resolve.json` per lane with formulas and computed values.
- Log lines shaped as `M12 native vertex path resolved` for valid lanes.
- Log lines shaped as `M12 native vertex path blocked` only for intentional invalid lanes.

Acceptance:

- Valid lanes produce `vertex_range_oob=0`.
- Intentional OOB lanes block with exact cause.
- Direct and ExecuteIndirect paths agree on resolved draw state.
- The previous generic `skipping unsafe DrawIndexedInstanced` warning is no longer the diagnostic for valid work.

### Phase 3 — Native Metal binding for resolved vertex/index draws

Goal:

Bind resolved D3D12 IA state to Metal deterministically without compact-slot races, stale stride assumptions, or duplicated offsets.

Code changes:

- Preserve D3D12 input slot numbers in the native path: Metal binding index must be traceable back to D3D12 `InputSlot`.
- If using Metal vertex descriptors, include effective D3D12 stride/layout in PSO/cache keys.
- If using shader-side vertex pulling, bind a raw D3D12 slot table and draw-argument block; do not compact slots for correctness proofs.
- Encode indexed draws with:
  - Metal index-buffer byte offset = IBV resource offset + `StartIndexLocation * indexSize`
  - Metal `baseVertex` = D3D12 `BaseVertexLocation`
  - Metal `baseInstance` = D3D12 `StartInstanceLocation`
- Add hard validation that Metal `indexBufferOffset` is aligned to index size.
- Add per-command-buffer retained-resource entries for every resolved MTLBuffer/MTLTexture/Pipeline/argument buffer used by the draw.

Mini-game proof:

- Load `m12_fresh_game.exe` with Phase 1 lanes after native Metal binding is enabled.
- Run repeated PSO reuse and dynamic-stride draws for at least 600 frames.

Validity artifacts:

- Per draw: D3D12 slot, Metal slot, MTLBuffer handle, offset, size, stride, resource-retain ID.
- Readback proves geometry remains correct after repeated PSO reuse.
- Log scan proves no fallback, no draw skip, no Metal command-buffer error.

Acceptance:

- Multi-slot, dynamic-stride, base-vertex, start-instance, and ExecuteIndirect lanes pass readback.
- No compact-slot fallback or CPU vertex workaround is used as a pass condition.

### Phase 4 — Fail-closed tessellation gate before native tessellation

Goal:

Remove VS/PS-only tessellation fallback from the success path immediately, even before full native tessellation exists.

Code changes:

- In `d3d12_pipeline_state.cpp`, replace green-path `D3D12 tessellation fallback` behavior with a hard `native_tessellation_required` PSO state for HS/DS PSOs.
- Capture shader hashes, HS/DS byte sizes, patch topology/control-point count, and PSO descriptor into an artifact.
- Ensure draws using unsupported HS/DS PSOs do not encode a fake VS/PS-only draw.

Mini-game proof:

- Load `m12_fresh_game.exe` with the current tessellation-shaped lane.
- Expected interim result: lane reports `native_tessellation_required` and no fallback draw is encoded.
- Verify following non-tessellation lanes still render, proving the block does not poison the command buffer.

Validity artifacts:

- `native_tessellation_required.json` with PSO/shader/topology metadata.
- Log scan: `D3D12 tessellation fallback=0`.
- Log scan: no fake VS/PS-only PSO success for HS/DS input.

Acceptance:

- Fallback count is zero.
- Unsupported tessellation is blocked explicitly.
- No CPU tessellation or VS/PS-only downgrade can be counted as a pass.

### Phase 5 — Implement `d3d12_native_tessellation_path.*`

Goal:

Render D3D12 patch/tessellation geometry on the GPU using Metal tessellation or a GPU prepass that feeds Metal patch rendering.

Code changes:

- Add native tessellation metadata extraction:
  - D3D12 patch-list topology and control-point count
  - HS control-point output shape
  - HS patch-constant/tessellation-factor output shape
  - DS domain and evaluation shape
  - partitioning, winding/order, factor format, and max factor
- Implement one native GPU strategy:
  - preferred: translate HS/patch constants into a Metal tessellation-factor buffer and use `setTessellationFactorBuffer` + `drawPatches`/`drawIndexedPatches`
  - acceptable intermediate GPU strategy: compute prepass generates Metal-compatible factor/control-point buffers, then native Metal patch draw consumes them
- Add patch draw encoding for indexed and non-indexed patches.
- Keep unsupported shapes fail-closed with `native_tessellation_unsupported` until mapped.

Mini-game proof:

- Add and load minimal quad/triangle patch HS/DS scenes in `m12_fresh_game.exe`.
- Include a progressive RGB tessellated triangle lane that renders frame-by-frame from a single lit pixel/point-sized patch output through increasing tessellated coverage to a complete RGB tessellated triangle. The lane must vary tessellation factor and/or control-point color/position over frames and must not satisfy the proof by presenting a complete triangle immediately.
- Include indexed and non-indexed patch variants.
- Include a tessellation-factor debug/readback buffer.
- Include per-patch and per-control-point validation.
- Include one intentionally unsupported patch shape that must fail closed.

Validity artifacts:

- Visible/readback proof of tessellated output.
- Per-frame RGB tessellated triangle artifacts: expected/actual sample colors, frame index, lit-pixel/coverage count, monotonic-growth verdict, control-point values, tessellation factor, and motion/color hash.
- Factor-buffer readback or debug dump.
- `native_tessellation_path resolved` logs with control-point count and factor metadata.
- Log scan: `D3D12 tessellation fallback=0`.

Acceptance:

- Native tessellation mini-game draw is visibly/readback correct on GPU.
- Progressive RGB vertex and tessellated triangle lanes both pass over a bounded frame sequence, with coverage growing from seed pixel/point to full triangle, giving a real-world frame-by-frame render-loop proof.
- Elden-like HS/DS PSOs no longer compile as VS/PS-only render PSOs.
- Unsupported tessellation blocks explicitly and never renders fallback.

### Phase 6 — Compute encoder encode failure hardening

Goal:

Make `M12 compute encoder encode failed` reproducible, diagnosable, and absent from valid proof runs.

Code changes:

- Add a compute dispatch resolver parallel to the native vertex resolver:
  - compute root signature snapshot
  - descriptor heap/root table epoch
  - root CBV/SRV/UAV descriptors
  - argument-buffer layout and required slots
  - bound resources and usage declarations
  - dispatch dimensions
- Convert `comp.encodeCommands(chain_head)` failure into a structured artifact containing PSO pointer/hash, CS hash, argument table qwords, missing resources, GPUVAs, resource states, and command-buffer status/error.
- Add command-buffer poison prevention: after an encode failure, end/abort the current encoder safely and do not continue as if the command buffer is valid.

Mini-game proof:

- Load `m12_fresh_game.exe` with descriptor mutation across compute dispatches.
- Add valid compute after heavy graphics PSO churn.
- Add intentional invalid compute descriptors that fail closed.

Validity artifacts:

- `native_compute_resolve.json` per dispatch lane.
- `M12 compute encoder encode failed=0` for valid lanes.
- Intentional invalid lanes identify missing/invalid binding exactly.

Acceptance:

- Valid compute lanes pass after graphics stress.
- Invalid compute lanes fail closed and do not poison later draws/dispatches.

### Phase 7 — Command/resource lifetime and DRED-style breadcrumbs

Goal:

Prevent progressive slowdown, invalid-resource faults, page faults, and hard-to-localize command-buffer failures.

Code changes:

- Add per-command-buffer retained-resource lists for every MTL object referenced directly or through argument buffers:
  - buffers
  - textures
  - samplers
  - heaps
  - pipelines
  - argument buffers
  - transient draw/dispatch buffers
  - drawables
- Release retained resources only in command-buffer completion handlers.
- Add M12 breadcrumbs:
  - before/after draw
  - before/after dispatch
  - before/after copy/barrier
  - PSO/shader hashes
  - IA/descriptor/resource-state summaries
- Enable Metal command-buffer encoder status in diagnostic/proof profiles and record `MTLCommandBufferErrorDomain`, error code, encoder labels, and M12 breadcrumbs.
- Audit `MakeTransientBuffer` use so transient memory cannot be recycled before GPU completion.

Mini-game proof:

- Load `m12_fresh_game.exe` for a long bounded stress run using native vertex, native/fail-closed tessellation, compute, descriptor mutation, transient buffers, and readback.
- Force resource churn while keeping valid D3D12 behavior.

Validity artifacts:

- In-flight command-buffer IDs and retained-resource counts.
- Completion-handler release logs.
- No invalid-resource/page-fault/timeout command-buffer errors.
- Stable frame/present timing within bounded thresholds.

Acceptance:

- No valid stress lane emits Metal command-buffer errors.
- Resource retention proves objects outlive GPU use.
- No progressive slowdown is observed in the bounded proof window.

### Phase 8 — Backpressure, drawable, and WindowServer safety

Goal:

Keep the renderer bounded so a failed proof exits safely instead of escalating into WindowServer/system instability.

Code changes:

- Bound in-flight command buffers and frames.
- Bound drawable acquisition; handle nil drawables as a controlled proof failure.
- Do not disable drawable timeout in proof/launch paths.
- Add frame/present stall detectors.
- Add emergency abort triggers for:
  - fallback string
  - vertex OOB string
  - compute encode failure
  - Metal command-buffer error
  - repeated nil drawables
  - present stall threshold
  - WindowServer/AGX/watchdog warning escalation

Mini-game proof:

- Load `m12_fresh_game.exe` for a long present stress run.
- Run under the same monitor that will be used for any future launch request.

Validity artifacts:

- Queue depth, drawable wait time, frame time, present count, abort-trigger counters.
- Host log scan for WindowServer/watchdog evidence within the proof window.

Acceptance:

- Proof remains bounded and exits cleanly.
- No new WindowServer timeout/reboot evidence is generated.
- Abort triggers work before system instability can escalate.

### Phase 9 — Shader replay and stale-sidecar guardrail

Goal:

Keep prior shader fixes from regressing while preventing shader audits from hiding runtime geometry/resource failures.

Code changes:

- Keep exact Elden DXIL/MSL replay gates.
- Keep post-launch `.msl.err.txt` freshness checks.
- Classify stale sidecars by mtime and paired `.msl` freshness before counting active errors.
- Keep shader replay separate from native vertex/tessellation acceptance; shader compile cleanliness is necessary but not sufficient.

Mini-game/probe proof:

- Run shader replay/corpus proof in the same final proof bundle as mini-game proof.
- Load `m12_fresh_game.exe` after replay to ensure runtime proof still passes.

Validity artifacts:

- Full cache replay summary.
- Active-vs-stale MSL sidecar inventory.
- Proof summary showing runtime gates and shader gates separately.

Acceptance:

- Full cache replay remains clean.
- `post_launch_msl_err_count=0` for active artifacts.
- Historical sidecars are reported but do not mask runtime failures.

### Phase 10 — Final prelaunch package and explicit user approval gate

Goal:

Produce a single, source-backed launch-readiness artifact before asking for any commercial launch.

Code changes:

- Add a final proof bundle generator that collects:
  - mini-game native vertex proof, including the progressive seed-pixel-to-full-triangle RGB vertex lane
  - native/fail-closed tessellation proof, including the progressive seed-pixel-to-full-triangle RGB tessellated lane once native tessellation is implemented
  - compute proof
  - command/resource lifetime proof
  - backpressure proof
  - shader replay proof
  - `/mtsp/prepare` runtime identity proof
  - PR CI status for the latest pushed commit
  - hard-fail string scan
- Add launch monitor config with immediate abort triggers for all hard-fail strings.
- After every push to the PR branch, query GitHub PR checks for the pushed commit and record pass/fail/pending status. Treat failing PR CI as a blocker to readiness, even when local offline proof passes.

Mini-game/probe proof:

- Load `m12_fresh_game.exe` through the full proof harness for the final bounded run.
- Require the progressive RGB vertex triangle lane and the native/fail-closed tessellation lane outputs in the final bundle; once Phase 5 lands, require the progressive RGB tessellated triangle lane as green too. The final bundle must include per-frame coverage counts proving the triangle was drawn out over time rather than presented whole immediately.
- Run `/mtsp/prepare` for Elden only after all mini-game/probe gates are green.
- Do not launch Elden as part of this phase without explicit user approval.

Validity artifacts:

- One final readiness directory under `06-results/in-progress` or `06-results/completed`.
- `READINESS.md` with exact counters, PR CI check results for the latest push, and links to logs.
- Runtime hash proof that the PR runtime is the one staged.

Acceptance:

- Every hard gate is green.
- Latest pushed PR commit has passing PR CI, or any pending/failing check is explicitly listed as a blocker with log evidence.
- Any unsupported native tessellation shape is clearly listed as blocked and not fallback-rendered.
- User approval is requested only after local proof and PR CI accounting are complete.

## Phase summary table

| Phase | Goal | Mini-game/probe proof | Must be zero for valid lanes |
|---|---|---|---|
| 0 | Wire no-fallback gates | short `m12_fresh_game.exe` smoke | fallback/draw-skip counters cannot pass |
| 1 | Add IA truth-table lanes | indexed/base/stride/instance/indirect scenes + progressive RGB vertex triangle reveal | valid-lane `vertex_range_oob` |
| 2 | Implement native vertex resolver | resolver JSON matches lane expectations | valid-lane OOB/block |
| 3 | Bind native vertex/index to Metal | 600f repeated draw/readback proof | compact-slot fallback, draw skip, Metal error |
| 4 | Remove tessellation fallback success | HS/DS lane blocks explicitly | `D3D12 tessellation fallback` |
| 5 | Native GPU tessellation | patch draw readback/factor proof + progressive RGB tessellated triangle reveal | fallback, CPU tessellation, fake VS/PS draw |
| 6 | Harden compute encode | compute descriptor/churn lanes | compute encode failure |
| 7 | Fix lifetime/breadcrumbs | resource churn stress | invalid resource/page fault/timeout |
| 8 | Bound backpressure | present/drawable stress | stall/reboot/WindowServer warning |
| 9 | Keep shader guardrails | replay + mini-game proof bundle | active MSL errors |
| 10 | Package launch readiness | full proof + `/mtsp/prepare` | all hard gates |

## Immediate implementation order

1. Phase 0 gate wiring.
2. Phase 1 mini-game lanes, because they define proof before runtime rewrites.
3. Phase 2 native vertex resolver.
4. Phase 3 Metal binding and retention for vertex/index draws.
5. Phase 4 fail-closed tessellation gate to remove fallback success immediately.
6. Phase 5 native GPU tessellation implementation.
7. Phase 6 compute encode resolver/artifacts.
8. Phase 7 command/resource lifetime hardening.
9. Phase 8 backpressure/window safety monitor.
10. Phase 9 shader guardrail bundle.
11. Phase 10 final prelaunch package.

## Open risks

- Metal tessellation is not a one-to-one D3D12 HS/DS mapping. A GPU prepass may be required to generate Metal-compatible factor/control-point buffers.
- D3D12 dynamic VB strides conflict with Metal pipeline-state vertex descriptor assumptions unless M12 uses vertex pulling or includes stride/layout in pipeline keys.
- Some index buffers may not be CPU-readable when sampling min/max indices. The resolver needs both a debug/proof sampling path and a non-mapping production path.
- Resource lifetime bugs may only appear under long PSO/draw/dispatch pressure. Bounded stress tests must run longer than five-frame smoke proofs.
- The final crash did not preserve a direct AGX panic backtrace. Future bounded launch monitors must gather better GPU/WindowServer evidence if a live launch is ever approved.

## Definition of done

The roadmap work is complete when this document is committed to the PR. Runtime implementation is complete only when:

- Native vertex/index mini-game proofs pass with no draw skips.
- Native tessellation proof passes on GPU, or unsupported tessellation blocks explicitly without rendering fallback.
- Compute encode proof passes.
- Command/resource lifetime stress proof passes.
- Backpressure/window safety proof passes.
- Exact shader replay remains clean.
- Elden prelaunch gates pass.
- The user explicitly approves any next commercial launch.
