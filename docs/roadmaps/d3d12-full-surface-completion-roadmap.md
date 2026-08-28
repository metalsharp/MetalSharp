# D3D12 Full-Surface Completion Roadmap

**Created:** 2026-08-27  
**Status:** Proposed — not complete; this roadmap supersedes the scoped
FL12_2/SM6.7/DXR completion claim for purposes of full API coverage.  
**Predecessor:** [D3D12 FL12_2 / SM6.7 / DXR completion roadmap](d3d12-fl12_2-sm67-dxr-completion-roadmap.md)  
**Target:** MetalSharp's vendored DXMT D3D12/DXGI/WineMetal runtime  
**Reference contract:** `tools/d3d12-metal-sdk/contracts/agility-1.619.3-contract.json`  
**Proof host:** Apple M4, Metal 4, macOS, Xcode 27 beta 6 (`27A5252f`, Metal
`32023.921.5`), MetalSharp Wine 11.5

---

## 1. Purpose and changed acceptance bar

The predecessor roadmap completed a deliberately scoped, behavior-backed
feature-level 12_2 surface. It correctly keeps unsupported behavior from
silently succeeding, but that is not the final acceptance bar for this
roadmap.

This roadmap has a stricter objective:

> Every legal D3D12/DXGI operation in the declared target surface must execute
> with its documented semantics. `E_NOTIMPL`, `DXGI_ERROR_UNSUPPORTED`, a
> false/zero capability report, an empty command method, or `S_OK` with no
> observable effect is not an acceptable implementation of a legal request.

This does **not** mean invalid input should succeed. Normal D3D12 validation
still returns the documented error for null pointers, malformed descriptors,
invalid states, unsupported enum values, exhausted resources, and other
invalid requests. The distinction is:

- **Invalid request:** reject it according to the Windows contract.
- **Valid request in the declared surface:** implement it; do not reject it as
  a backend limitation.
- **Valid request outside the declared surface:** it may not be called part of
  this completion. The target surface must be explicitly expanded first rather
  than hiding the gap behind a capability report.

The current 14-entry unsupported ledger therefore becomes a work queue, not a
permanent exemption. Its `unsupported` and `limited_to_proven_probe` entries
must be removed only after positive behavior, exact readback/side-effect
checks, and negative validation pass.

### 1.1 What “full surface” means here

The target is the functional D3D12/DXGI surface represented by the Agility
1.619.3 reference contract and the vendored Windows headers, including:

1. Core D3D12 device, object, resource, heap, descriptor, fence, query,
   pipeline, queue, command-list, swapchain, and state-object interfaces.
2. Feature levels `11_0`, `11_1`, `12_0`, `12_1`, and `12_2`, with all feature
   reports backed by the behavior they advertise.
3. SM5.x DXBC/AIR and SM6.0 through SM6.7 DXIL paths, including every opcode,
   intrinsic, stage, resource dimension, legal view, and diagnostic in the
   declared shader-model target.
4. All D3D12 feature families currently represented in the SDK contract:
   resources, residency, sparse/tiled memory, barriers, VRS, MSAA, ROVs,
   conservative rasterization, view instancing, mesh/amplification shaders,
   work graphs, DXR, video, protected resources, DSR, caches, and Agility
   extensions.
5. DXGI 1.6 factory, adapter, output, resource, surface, display, sharing,
   event, overlay, duplication, and presentation behavior.
6. Agility configuration, compiler/cache, pipeline/state-object cache, debug,
   DRED, tools, lifetime, sharing-contract, and newer interface methods in the
   1.619.3 contract.
7. ABI parity across x86_64 PE, x86_64 Unix, WOW64 call paths, Wine loader
   routing, and all matching Winemetal exports.

The 145 interfaces listed by `agility-1.619.3-contract.json` are an inventory
input, not proof that the current implementation already exposes all of them.
The first phase produces a method-level inventory and resolves every missing
method explicitly.

### 1.2 Required implementation strategies

Metal does not have a one-to-one primitive for every Windows API. A valid
implementation may use one of these providers, selected per operation:

- Native Metal/Metal 4 acceleration, sparse memory, rasterization, or
  synchronization.
- Metal compute/replay emulation with an explicit command and resource model.
- A CPU reference/software provider for operations with no Metal equivalent.
- macOS system facilities such as VideoToolbox, CoreVideo, IOSurface,
  CoreAnimation, DisplayLink, Mach shared-memory/port transport, and windowing
  APIs.
- A separate compatibility provider when an operation requires security or
  hardware semantics not exposed by ordinary Metal memory.

A provider must preserve the D3D12-visible semantics, ordering, errors,
resource lifetime, and data values. Ordinary `MTLStorageModePrivate` memory is
not automatically a protected-resource implementation. A fake CPU return is
not automatically WARP. A cache hit is not a compiled pipeline unless the
pipeline is executable.

If the M4/macOS host cannot provide a required security or hardware guarantee,
that is a platform feasibility blocker requiring a real provider or a target
host/backend change. It must not be converted into a silent downgrade and must
not be declared complete with a fail-closed return.

---

## 2. Current baseline: complete, limited, and absent

The predecessor's final evidence remains useful baseline evidence:

- FL12_2 aggregate gate: 31/31 queries, 31/31 behaviors, 22/22 identity,
  zero blockers.
- Full source build: 156/156 targets.
- Final runtime staging: 18 artifacts, zero failures and zero hash mismatches.
- Scoped M12 proof: explicit FL12_2 creation, 81 presented frames, exact
  `bright=2009`, `chroma=2009` readback.
- D3D10/D3D11 source-staged clear/copy/readback regression passes.

Those numbers prove the **scoped** contract only. They do not close this
roadmap. The following rows are the starting gap inventory.

### 2.1 Existing ledger rows that become blockers

| Area | Current state | Full completion required |
| --- | --- | --- |
| DXR | Tier 1.1 behavior is proven for a focused matrix; mixed children are flattened into consuming TLAS instances | Native mixed BLAS geometry, every legal geometry/build/update/instance shape, cross-process serialization, independent collection linking, complete state-object growth, and all table/stride/local-data combinations |
| Mesh shaders | Tier 1 direct/indirect and selected AS/MS paths pass | All payload sizes, thread-group shapes, resource classes, barriers, derivatives, array ranges, amplification patterns, and mesh per-primitive VRS |
| Work graphs | No program, memory, node, or scheduler implementation | Full graph creation/properties, backing memory, node dispatch, record routing, synchronization, barriers, and execution |
| Video | No D3D12 video backend | Decode, encode, process, motion, extension commands, metadata, profiles, formats, and VideoToolbox integration |
| Protected resources | Creation paths return `E_NOTIMPL` or reject protected sessions | Protected session objects, protected heaps/resources/command lists, synchronization, and an actual protected-memory/security provider |
| DSR | No DSR device factory/backend | DSR device, resource/state model, scaling/upscale execution, quality modes, and presentation integration |
| State objects | Foundational DXR state objects pass | Independently linked collections, new-library growth, every valid table layout/count/stride/local-root form, and complete cache serialization |
| Amplification shaders | Four-byte payload and narrow resource proof | Full AS-to-MS payload/resource/barrier/thread-group/pattern coverage and derivatives |
| Node shaders | Absent as part of work graphs | Full node shader execution and graph node properties |
| Stream output | No output buffer recording/replay | Declarations, strides, multiple streams, counters, overflow behavior, capture buffers, and downstream consumption |
| Sparse/reserved resources | Focused native sparse textures/buffers pass | All legal dimensions, formats, packed/partial mips, physical page ownership, mapping copies, queue ordering, residency, aliases, and `CopyTiles` |
| Shared resources | Process-local registry only | Portable cross-process resource/heap/event transport, names, LUIDs, synchronization, lifetime, and security validation |
| Geometry shaders | Narrow SM5 vertex/geometry proof | General topology, resource, stream, primitive, state, and multi-stage coverage |
| Hull/domain shaders | One native tessellation proof shape | All patch layouts, partitioning, topologies, factors, resources, indexing, state, and shader conversion paths |

### 2.2 Feature and query gaps

The runtime still returns false, zero, partial, or otherwise conservative values
for feature families that this roadmap must implement before reporting support:

- ROVs and ordered pixel UAV access.
- Double precision and minimum-precision shader behavior.
- Programmable sample positions.
- View instancing and barycentrics.
- 64-KB MSAA alignment and native 16-bit shader operations.
- Full RT-array index range and derivatives in mesh/amplification stages.
- Mesh per-primitive VRS.
- Triangle fan, dynamic strip-cut, dynamic depth bias, GPU upload heaps.
- Non-normalized samplers and manual write tracking.
- Options 13, 17, 20, and other zeroed extension fields.
- Work graphs, node shaders, `SampleCmp` gradient/bias, extended command info.
- Tight alignment, hardware copy, async commands, fence barriers, and barrier
  layouts.
- Shader-cache ABI, MLIR, linear algebra, shader execution reordering, and
  byte-offset views.
- Protected-resource support.

The current contract also needs reconciliation: `D3D12_OPTIONS.TiledResourcesTier`
is reported as Tier 3 while the unsupported ledger still says the full Tier 3
surface is not proven. This roadmap requires one authoritative state and a
full matrix, not contradictory documents.

### 2.3 Explicit no-op and false-success audit starting points

The following current paths are implementation blockers, even where they are
not in the unsupported ledger:

- `d3d12_command_list.cpp`: `SOSetTargets`, `DiscardResource`, `SetPredication`,
  command-list markers/events, `AtomicCopyBufferUINT`,
  `AtomicCopyBufferUINT64`, `SetSamplePositions`, `SetViewInstanceMask`,
  `SetProtectedResourceSession`, `InitializeMetaCommand`, and
  `ExecuteMetaCommand`.
- `d3d12_resource.cpp`: `Unmap` and private/default-resource
  `ReadFromSubresource`/`WriteToSubresource` behavior.
- `d3d12_device.cpp`: existing-heap opens, protected sessions, lifetime
  trackers, meta commands, background processing, residency/priority
  bookkeeping, state-object database serialization, and unconditional device
  removal/stable-power responses.
- `d3d12_command_queue.cpp`: CPU-zero `GetClockCalibration` and any queue
  event path that logs without applying the required GPU/debug operation.
- `d3d12_dxgi_device.cpp`: `CreateSurface`, all-resident-only residency
  reporting, and empty `Trim`.
- `dxgi_factory.cpp`: CoreWindow/composition swapchains, shared-resource LUID,
  WARP/software adapters, and status registrations that allocate cookies but
  do not deliver notifications.
- `dxgi_output.cpp`: ownership, VBlank, display-surface transfer, frame
  statistics, duplication, overlays, and hardcoded display-format behavior.
- `dxgi_resource.hpp`: `CreateSubresourceSurface`.
- InfoQueue/debug/tool interfaces that accept filters or messages but do not
  store, retrieve, callback, or apply them.

The final scanner must prove that every one of these paths is either backed by
real behavior or has been replaced by a complete provider.

---

## 3. Evidence model and contracts

### 3.1 New authoritative artifacts

Add these repository-owned artifacts before broad implementation begins:

- `contracts/d3d12-full-surface-contract.json` — method/feature contract.
- `contracts/d3d12-full-surface-matrix.json` — positive and negative cases.
- `contracts/d3d12-provider-contract.json` — native, emulated, CPU,
  VideoToolbox, display, shared-memory, and protected providers.
- `contracts/d3d12-interface-census.json` — IIDs, vtable order, methods,
  header provenance, and implementation owner.
- `contracts/d3d12-no-op-policy.json` — forbidden empty bodies, forbidden
  `S_OK` no-op patterns, and allowed validation-only returns.
- `results/d3d12-full-surface-gate-<profile>.json` — aggregate evidence.
- `results/d3d12-full-surface-gate-<profile>.md` — human-readable summary.

The existing `feature-support-contract.json`, `unsupported-api-ledger.json`,
`risky-stub-ledger.json`, `agility-1.619.3-contract.json`, and probe matrix
remain inputs. The new contract becomes the merge authority for the expanded
scope.

### 3.2 Required record for every method or feature

Every contract row must identify:

- API/IID, method or feature ID, header version, and exact ABI signature.
- Valid input domain and invalid-input cases.
- Expected HRESULT/return value and output initialization rules.
- Resource/state side effects and queue ordering.
- Provider selected on M4/Metal 4 and fallback provider if applicable.
- Positive behavior probe and exact readback/observable side effect.
- Negative validation probe.
- Lifetime, reset/reuse, multi-queue, and concurrency coverage.
- x86_64 PE, x86_64 Unix, WOW64, and call-table provenance where relevant.
- Runtime/SDK/hash inputs and disposable-prefix identity.

A query-only result cannot satisfy a behavior row. A compile-only result cannot
satisfy an execution row. A successful API return with no observable side
 effect cannot satisfy any row.

### 3.3 Common proof requirements

Every phase's focused probe must include, where applicable:

1. Creation and correct output initialization.
2. Nominal execution with deterministic data.
3. Boundary dimensions, counts, offsets, strides, formats, and alignment.
4. Invalid arguments and incompatible state rejection.
5. Resource release before GPU completion where D3D12 permits it.
6. Command-list close/reset/reuse and queue synchronization.
7. Exact bytes, pixels, counters, timestamps, events, or callback records.
8. A fresh source-staged Wine 11.5 run with the matching runtime.
9. Captured evidence copied out before Wine/prefix deletion.

---

## 4. Sequential implementation phases

The phases are deliberately ordered. Do not promote a later feature or widen
its report while an earlier provider, ABI, memory, or synchronization phase is
red.

### Phase 0 — Freeze the full inventory and downgrade the completion claim

**Goal:** Make the expanded target measurable and prevent the scoped gate from
being mistaken for full completion.

**Work:**

- Generate the interface census from the vendored headers and Agility 1.619.3
  contract, covering all 145 listed interfaces and every method.
- Enumerate direct `E_NOTIMPL`, `DXGI_ERROR_UNSUPPORTED`, empty method bodies,
  uninitialized outputs, unconditional `S_OK`, false/zero capability fields,
  and capability reports that exceed their probes.
- Reconcile the two current roadmap scopes and label the existing 31/31 gate as
  `scoped_fl12_2`, not `full_surface`.
- Build a method-to-provider map with no unassigned legal method.
- Add a CI inventory check that fails when a new interface method appears
  without a contract row and owner.

**Exit gate:**

- Zero unknown methods in the census.
- Zero unexplained gaps between headers, QI/vtables, contracts, and source.
- A full gap report is committed before feature implementation resumes.

### Phase 1 — Build the provider, synchronization, and capability architecture

**Goal:** Establish the infrastructure needed for features with no direct
Metal equivalent.

**Work:**

- Version the WineMetal PE/Unix call tables and add every required primitive
  with normal/WOW64 parity.
- Add a provider interface for native Metal, compute emulation, CPU reference,
  VideoToolbox/CoreVideo, display/windowing, cross-process sharing, and
  protected resources.
- Add one timeline model for D3D12 fences, queue waits/signals, command-buffer
  completion, resource visibility, and CPU callbacks.
- Add resource-state/layout tracking independent of Metal encoder lifetime.
- Add provider selection and capability negotiation based on actual host
  features, not compile-time optimism.
- Add deterministic fault propagation: failed native encoding must surface as a
  failed command execution or API call, never a completed command buffer.

**Exit gate:**

- Native/Unix ABI census passes for every current and new export.
- Direct, compute, copy, and any future video queues can signal/wait on the
  same timeline model.
- A provider test can execute one resource write, barrier, readback, release,
  and callback on every available provider.

### Phase 2 — Complete COM objects, interface exposure, and lifecycle

**Goal:** Make the complete object model correct before adding more GPU work.

**Work:**

- Implement exact `QueryInterface` exposure and vtable compatibility for all
  declared D3D12/DXGI/Agility interfaces.
- Complete private-data byte copying, interface ownership, names, destruction
  notifications, child-object lifetime, and parent/device links.
- Implement InfoQueue storage/retrieval filters, message limits, breaks,
  mute state, callbacks, and exact buffer-size behavior.
- Implement DRED/device-removed data, diagnostic callbacks, and live-object
  reporting against the internal object registry.
- Implement lifetime owners/trackers and destruction callbacks.
- Implement device factory/configuration/experimental-feature interfaces and
  reject only malformed requests.
- Add COM ABI tests for every interface: QI, AddRef/Release, method dispatch,
  output initialization, and object destruction.

**Exit gate:**

- Interface census has no missing QI or vtable owner.
- Every object category survives create/use/release/recreate cycles.
- InfoQueue, DRED, lifetime, and private-data tests pass under x86_64 and
  WOW64.

### Phase 3 — Finish resources, heaps, virtual memory, residency, and sharing

**Goal:** Remove resource-shape and backing restrictions from the declared
surface.

**Work:**

- Complete all legal D3D12 resource dimensions: buffers, 1D/2D/3D, arrays,
  cubes, MSAA, mip chains, planes, typeless, depth/stencil, compressed, and
  castable formats.
- Implement complete `GetResourceAllocationInfo*`, footprints, format planes,
  alignment, copy layouts, and format capability queries.
- Implement CPU mapping for upload/readback/default resources, written-range
  tracking, `Unmap`, `ReadFromSubresource`, `WriteToSubresource`, and staging
  transfers with correct row/slice pitches.
- Finish heaps and placed resources at every legal offset/alignment, including
  aliasing and overlapping lifetime rules.
- Replace all-resident bookkeeping with real residency state, priorities,
  `MakeResident`, `Evict`, `EnqueueMakeResident`, trim, reclaim, and resource
  residency queries.
- Finish reserved resources and Tier 3 sparse behavior:
  `GetResourceTiling`, standard and packed/partial mips, 1D/2D/3D/array/cube
  layouts, all supported formats, physical page ownership, update/copy tile
  mappings, cross-queue ordering, unmap zeroing, aliasing, and `CopyTiles`.
- Implement portable cross-process shared resources, heaps, fences/events,
  named handles, adapter LUID lookup, and lifetime/security checks using
  platform-backed transport rather than a process-local map.
- Implement `OpenExistingHeapFromAddress` and file-mapping paths with proper
  ownership and validation.

**Exit gate:**

- Exhaustive resource matrix passes creation, map/copy/readback, alias,
  residency, release, and invalid-input cases.
- Every advertised sparse tier case has physical-page evidence.
- Two independent processes can share a resource, synchronize it, read it,
  and close it without leaks or stale handles.

### Phase 4 — Complete queues, command recording/replay, barriers, and indirect work

**Goal:** Ensure every legal command is recorded, retained, ordered, and
replayed with no dropped operation.

**Work:**

- Add command records and replay for every graphics/compute/copy/video command
  list operation, including:
  `SOSetTargets`, `DiscardResource`, predication, markers/events, atomic buffer
  copies, sample positions, view-instance masks, protected sessions, metadata,
  and all queue commands.
- Implement predication with correct conditional execution for draw, dispatch,
  copy, and indirect work.
- Implement stream-output capture and counter/overflow behavior.
- Implement atomic copy semantics and dependent-resource ranges.
- Implement programmable sample-position state and its interaction with MSAA.
- Implement view-instancing masks, viewport/RT-array routing, and per-view
  state.
- Implement complete enhanced/legacy barriers, layout transitions, UAV
  ordering, render-pass splits, cross-queue fences, and visibility.
- Implement every command-signature argument type and ExecuteIndirect count,
  bounds, stride, and nested signature behavior.
- Implement queue priorities, markers, debug groups, timestamps, clock
  calibration, VBlank-related queue waits, and event callbacks.
- Make command-list close/reset/reuse and allocator ownership match D3D12
  ordering and error behavior.

**Exit gate:**

- Command inventory reports zero dropped legal commands.
- A replay probe compares every command's expected side effect with exact
  readback/event/timestamp evidence.
- No queue method returns success solely because it logged the call.

### Phase 5 — Complete the shader compiler and SM5.x–SM6.7 execution surface

**Goal:** Replace the reduced synthetic corpus with complete declared shader
semantics.

**Work:**

- Define exact supported shader-model range and stage matrix for SM5.x DXBC/AIR
  and SM6.0–SM6.7 DXIL; keep D3DCompile/DXC provenance in every result.
- Complete DXIL container/version/signature/root-signature/reflection parsing.
- Implement all declared DXIL opcodes and intrinsics rather than generating a
  default value when lowering is unsupported.
- Cover scalar/vector/matrix types, 16/32/64-bit arithmetic, conversions,
  atomics, barriers, memory scopes, derivatives, helper lanes, WaveOps,
  QuadOps, resource handles, descriptor heaps, root constants, all texture
  dimensions, arrays, cubes, MSAA, depth comparisons, typed/raw/structured
  buffers, and UAV counters.
- Implement diagnostics with stage/opcode/intrinsic/resource reasons and make
  pipeline creation fail only for malformed/invalid shaders, not due to an
  unimplemented legal operation in the declared model.
- Finish advanced SM6.7 texture behavior across applicable stages and views:
  programmable offsets, raw gather, `SampleCmpLevel`, gradient/bias forms,
  mip selection, borders, arrays, depth, and typed/castable views.
- Finish Int64 and atomic64 semantics for every declared resource class and
  operation without same-SIMD lock deadlock.
- Implement complete shader cache/compiler factory/session behavior, cache
  invalidation, pipeline-library serialization, and cache ABI reporting.
- Add a reference CPU evaluator for selected opcodes and a Windows reference
  comparison corpus for values and errors.

**Exit gate:**

- Every contract opcode/intrinsic row has positive and negative evidence.
- No generated MSL contains an unhandled opcode, placeholder value, skipped
  store, or skipped resource operation for a legal target shader.
- All shader stages compile, link, bind, execute, and read back exact values.

### Phase 6 — Complete graphics stages, rasterization, ROVs, VRS, MSAA, and formats

**Goal:** Make the full graphics pipeline behaviorally complete.

**Work:**

- Complete vertex, pixel, geometry, hull, domain, tessellation, amplification,
  mesh, line, point, patch, triangle-fan, and strip-cut topology paths.
- Generalize geometry shader conversion and replay across topology, resources,
  stream output, primitive restart, adjacency, and multi-stage state.
- Generalize hull/domain tessellation across patch counts, partition modes,
  winding, factors, indexing, resources, and all legal render states.
- Implement ROV semantics with Metal raster-order groups or a deterministic
  compute/replay provider, including interlock ordering and UAV visibility.
- Implement all conservative-rasterization rules: edge/inner coverage,
  top-left and degenerate triangles, winding, clipping, viewport/scissor,
  depth, blend, VRS, arrays, lines, and MSAA sample coverage.
- Implement full VRS Tier 2: every legal rate, image dimension/layout/upload,
  both combiner stages and axes, `PASSTHROUGH`/`OVERRIDE`/`MIN`/`MAX`/`SUM`,
  per-primitive values, mesh per-primitive VRS, viewport offsets, arrays,
  logical reconstruction, and lifecycle.
- Implement view instancing and barycentrics.
- Implement programmable sample positions and all MSAA raster/depth/resolve
  behavior.
- Expand writable MSAA to every advertised legal format, dimension, sample
  count, array, graphics/compute path, DSV interaction, resolve mode, and
  partial coverage case.
- Remove hardcoded format assumptions, including display format selection,
  typed UAV support, castable formats, compressed formats, planes, and
  format-specific atomics.
- Implement dynamic depth bias, native 16-bit operations, double precision,
  minimum precision, and all Options 13/15/16/17/19 fields whose support is
  reported.

**Exit gate:**

- CPU reference and GPU/provider readbacks agree for every raster matrix.
- ROV, VRS, conservative raster, view instancing, sample position, geometry,
  tessellation, and MSAA positive/negative probes pass.
- Feature queries are derived from the passing matrices, not set in advance.

### Phase 7 — Complete mesh, amplification, work graphs, and node shaders

**Goal:** Implement the programmable scheduling model beyond ordinary compute.

**Work:**

- Finish all Mesh Tier 1 AS/MS pipeline streams, payload sizes, thread-group
  shapes, resource classes, barriers, depth/stencil, blend, arrays, VRS,
  statistics, direct/indirect dispatch, and reset/reuse cases.
- Finish amplification shaders with arbitrary legal payloads, resources,
  barriers, dispatch patterns, derivatives, and interaction with mesh stages.
- Implement work-graph program objects, node/entrypoint properties, record
  alignment/size, backing memory requirements, local roots, node IDs, graph
  creation, dispatch, completion, barriers, and lifetime.
- Implement node shader execution with a persistent Metal compute scheduler,
  indirect dispatch queues, or CPU fallback where required. Preserve D3D12
  record ordering, work amplification, synchronization, and memory visibility.
- Implement `ID3D12WorkGraphProperties`, graph state objects, graph-related
  command signatures, and diagnostic/overflow reporting.
- Add multigraph, multi-node, recursive/fan-out, record-overflow, and
  cross-queue tests.

**Exit gate:**

- `D3D12_OPTIONS21.WorkGraphsTier` and all mesh/amplification fields are
  behavior-derived.
- Work graph/node probes execute nontrivial graphs and read back exact record,
  payload, resource, and ordering results.
- No work-graph or node method remains an absent interface, empty command, or
  placeholder property.

### Phase 8 — Complete DXR 1.0/1.1 without a narrowed matrix

**Goal:** Turn the current foundational DXR bridge into complete declared DXR
1.1 behavior.

**Work:**

- Implement one ABI-checked geometry descriptor path for native mixed triangle
  and procedural AABB geometry within a single BLAS; do not rely only on
  flattening child BLAS records into a TLAS.
- Cover all legal BLAS/TLAS geometry counts, layouts, transforms, flags,
  opacity/any-hit settings, update/refit combinations, compaction, postbuild
  info, scratch/result aliasing, and barriers.
- Complete raygen, miss, any-hit, closest-hit, intersection, callable, shader
  table, local-root, global-root, descriptor-table, sampler, stack-size,
  recursion, and pipeline-config semantics.
- Complete direct and indirect `DispatchRays` with all legal table counts,
  strides, offsets, dimensions, and argument-buffer layouts.
- Complete state-object collections, independent linking, import renaming,
  missing-export validation, `AddToStateObject`, new-library growth, and every
  valid collection/state-object composition.
- Make serialized AS data process-independent where D3D12 requires it. Use a
  documented portable representation and platform resource rehydration rather
  than embedding process-local pointers or Metal object identities.
- Prove source-resource release, queue synchronization, multi-TLAS lifetime,
  concurrent builds, and error recovery.
- Implement all state-object properties and shader identifiers, including
  program identifiers, complete stack sizes, and configured stack retention.

**Exit gate:**

- Full DXR contract passes with triangle, AABB, mixed-BLAS, multi-instance,
  state-object, collection, table, recursion, indirect, serialization,
  synchronization, and release/lifetime matrices.
- No DXR ledger row remains `limited_to_proven_probe`.
- `D3D12_OPTIONS5.RaytracingTier` is reported only from this full result.

### Phase 9 — Implement D3D12 video through a real media provider

**Goal:** Remove the absent D3D12 video surface.

**Work:**

- Implement VideoToolbox/CoreVideo adapters for decoder, encoder, processor,
  motion estimator, motion-vector heap, extension command, and all versioned
  interfaces in the Agility contract.
- Map D3D12 video profiles, levels, formats, resolutions, reference frames,
  rate control, metadata, output buffers, input surfaces, and color spaces.
- Bridge `ID3D12Resource` to CVPixelBuffer/IOSurface with explicit ownership,
  row/plane layout, synchronization, and conversion rules.
- Implement video command-list recording, barriers, predication, protected
  session association, query/capability methods, and asynchronous completion.
- Add H.264/H.265/AV1 and every host-supported codec/profile matrix, including
  unsupported-profile validation and exact decoded/encoded byte or pixel
  comparison.

**Exit gate:**

- Every video interface/method in the contract has a provider and test.
- Encode/decode/process round trips agree with a reference implementation.
- Queue, resource, metadata, and lifetime tests pass without a no-op path.

### Phase 10 — Implement protected resources and security-sensitive paths

**Goal:** Provide actual protected-resource semantics, not ordinary private
Metal memory under a protected-session interface.

**Work:**

- Define the security requirements for each protected D3D12 operation:
  CPU visibility, GPU-only access, process isolation, handle transfer,
  encryption, key lifetime, display path, and failure behavior.
- Inventory macOS/M4 primitives that can satisfy those requirements. Add a
  protected-memory provider, or a separate trusted execution/compatibility
  provider, with a documented security boundary and threat model.
- Implement `ID3D12ProtectedResourceSession`, versioned sessions, protected
  heaps/resources, command-list association, video association, and all
  `GetProtectedResourceSession` methods.
- Enforce protected/unprotected aliasing and sharing rules, access rights,
  session teardown, key/session lifetime, and device-removal behavior.
- Add security tests that attempt invalid CPU mapping, unauthorized handle
  open, cross-session use, unprotected aliasing, and post-session access.

**Exit gate:**

- An independent security review/test harness confirms the documented
  guarantee; ordinary `MTLStorageModePrivate` is not accepted as evidence.
- Every protected-session query and method has positive/negative behavior.
- The host capability contract records the provider and security guarantee.

This phase is a hard platform gate. If macOS/Metal cannot supply the required
security primitive, the roadmap is not complete on that host; the answer is a
real alternate provider or host target, not `E_NOTIMPL` hidden behind a report.

### Phase 11 — Implement DSR and advanced display scaling

**Goal:** Implement the DSR factory and execution semantics.

**Work:**

- Implement `ID3D12DSRDeviceFactory` and all DSR device/state interfaces in
  the target headers.
- Define DSR resource allocation, quality/mode selection, input/output sizes,
  motion/history dependencies, sharpening, HDR/color-space handling, and
  synchronization.
- Use Metal compute/MetalFX/CoreImage only where the resulting behavior
  matches the D3D12 contract; retain a deterministic compute fallback.
- Integrate DSR with swapchains, composition, frame latency, present, and
  resource lifetime.
- Add exact pixel/color-space/metadata comparisons at every supported scaling
  mode and invalid-mode rejection.

**Exit gate:**

- DSR factory, state, resources, execution, and presentation probes pass.
- No DSR method is absent, a no-op, or advertised without output evidence.

### Phase 12 — Complete DXGI factories, outputs, surfaces, presentation, and display

**Goal:** Remove the remaining DXGI compatibility gaps.

**Work:**

- Implement CoreWindow and composition swapchains with CoreAnimation/
  CAMetalLayer-backed surfaces and correct buffer ownership.
- Implement `IDXGIDevice::CreateSurface`, subresource surfaces, shared surface
  views, surface map/copy/readback, and resource adapter LUID lookup.
- Implement output ownership/release, real VBlank waits, display surface set/
  readback, frame statistics, gamma application, HDR/color-space negotiation,
  display-mode enumeration without hardcoded sRGB substitution, and display
  format conversion.
- Implement desktop duplication and `DuplicateOutput1` with IOSurface/
  DisplayLink capture, frame metadata, dirty regions, cursor/rotation/color
  space, synchronization, and lifetime.
- Implement overlay capability and composition paths, or a real composition
  provider that produces the advertised effect.
- Implement software/WARP adapter behavior through the CPU provider, including
  adapter enumeration, device creation, feature reports, and deterministic
  rendering/copy results.
- Implement adapter-change, occlusion, stereo, and status event registration,
  delivery, validation, and unregister cleanup.
- Implement swapchain frame latency, tearing, fullscreen, present statistics,
  CoreWindow, composition, and `GetCoreWindow` behavior.

**Exit gate:**

- DXGI 1.6 factory/adapter/output/surface/swapchain/duplication tests pass on
  windowed, composition, headless, and software-provider paths.
- Every event cookie represents a real registration and callback lifecycle.
- No output or presentation method returns success without the documented
  effect.

### Phase 13 — Finish Agility, caches, diagnostics, tools, and configuration

**Goal:** Complete the modern interface surface instead of only the core draw
path.

**Work:**

- Implement compiler factory, compiler child/state-object, shader cache
  sessions, cache ABI, cache installer/application/component/explorer paths,
  invalidation, serialization, and version negotiation.
- Finish pipeline library and state-object database store/find/serialize/load,
  application metadata, object versioning, and corruption handling.
- Implement device factory/configuration, SDK configuration, experimental
  features, application identity, sharing contract, virtualization guest
  device, and SDK version behavior.
- Implement all debug interfaces, InfoQueue1 callbacks, DRED settings/data,
  debug command-list/queue/device assertions, tools, instrumentation, manual
  write tracking, trim callbacks, and pageable allocation tools.
- Implement queue priorities, stable power state, background processing,
  `RemoveDevice`, device tools, and all feature-query structs with real
  semantics.
- Complete `ID3D12Device13`–`ID3D12Device15` and later declared interfaces,
  including try-create views, trim callbacks, byte-offset views, and new
  allocation/view paths.

**Exit gate:**

- The 145-interface census has no compatibility-only placeholder method in the
  declared target.
- Cache round trips survive process restart and corrupted data is rejected.
- Debug/tool/configuration probes verify state changes, callbacks, and output
  sizes exactly.

### Phase 14 — Remove conservative reports and delete the unsupported ledger

**Goal:** Make capability reporting the consequence of complete behavior.

**Work:**

- Re-run the entire method/feature inventory and no-op scanner.
- For every current `unsupported` or `limited_to_proven_probe` row, either
  complete the implementation and promote it, or expand the provider/target
  until it is implemented. Do not leave a permanent exemption in the full
  surface contract.
- Remove `ROVsSupported=false`, partial Options10/14 values, zeroed extension
  structs, work-graph/video/protected/DSR/stream-output false reports, and all
  other conservative values only after their behavior gates pass.
- Derive every query from a generated behavior ledger. A query cannot be
  manually raised above the maximum passing matrix.
- Make unsupported/unknown feature IDs return the documented invalid-feature
  result; do not use zeroed `S_OK` responses as a compatibility shortcut.
- Replace the old unsupported ledger with a zero-gap completion ledger. Keep a
  separate invalid-input ledger for expected validation failures.

**Exit gate:**

- No legal target operation returns `E_NOTIMPL` or
  `DXGI_ERROR_UNSUPPORTED`.
- No legal target command is dropped.
- No capability report exceeds a positive behavior row.
- No empty method body or `S_OK` no-op survives the scanner except explicitly
  proven metadata-only methods whose state change is tested.
- The generated full-surface contract has zero blockers and zero limited rows.

### Phase 15 — Exhaustive validation, differential testing, and game coverage

**Goal:** Demonstrate that the full surface works outside one-purpose probes.

**Work:**

- Run positive and negative probes for every contract row from isolated Wine
  11.5 prefixes.
- Run a Windows reference comparison suite for HRESULTs, feature queries,
  footprints, resource bytes, shader outputs, presentation, video, and DXR.
- Run concurrency/fuzz tests for descriptors, command streams, shader bytecode,
  state objects, shared handles, mappings, and cache blobs.
- Run full D3D10/D3D11 regression suites, including exact clear/copy/readback,
  resource lifetime, presentation, and cross-route staging.
- Run multiple real D3D12 applications with captured launch methods and
  behavior logs. A real game is additional evidence, never a replacement for
  focused contract probes.
- Run long-duration frames, shader-cache cold/warm paths, device loss/recovery,
  process restart, cross-process sharing, queue starvation, memory pressure,
  video throughput, and display attach/detach tests.
- Verify no deferred/failed PSO is presented as a successful PSO and no command
  buffer is marked complete after provider failure.

**Exit gate:**

- Full matrix has no skipped legal case, no unexplained flaky case, and no
  provider-specific discrepancy outside the documented contract.
- D3D10/D3D11 and all D3D12 routes remain green.
- At least one independent consumer exercises each major provider: native
  Metal, emulation, CPU/WARP, video, display, shared, protected, DSR, work
  graph, and DXR.

### Phase 16 — Final reproducible staging and release delivery

**Goal:** Ship the full implementation without stale or mismatched runtime
artifacts.

**Work:**

- Clean-build the complete source tree with pinned Xcode/LLVM/MinGW inputs.
- Extend the manifest to include every PE/Unix/sidecar/provider artifact and
  verify SHA-256, architecture, exports, ABI, and dependency layout.
- Stage only manifest-selected runtime files into a disposable developer
  runtime; never copy Wine loader modules into an application directory.
- Run runtime doctor, ABI, hash, loader, full-surface, D3D10/D3D11, package, and
  clean-prefix gates.
- Run live M12 and representative real-game launches with selected runtime
  hashes recorded in the evidence.
- Remove generated binaries, build metadata, caches, logs, prefixes, shader
  dumps, and temporary probe artifacts from the commit.
- Update both roadmaps, the SDK README, contracts, compatibility matrix, PR
  summary, and release notes with exact evidence paths and provider/host
  requirements.
- Push the branch only after the working tree is clean and all required CI
  checks pass.

**Exit gate:**

- Full-surface aggregate gate: all contract rows pass, zero blockers, zero
  limited/unsupported rows.
- Clean 156-target baseline plus all new provider targets pass.
- PE/Unix/WOW64 ABI and hash checks pass.
- Fresh-prefix runtime and representative live launches pass.
- Working tree contains only intended source/docs changes.

---

## 5. Detailed gap checklists

This section is the implementation checklist extracted from the current audit.
Each item must become a contract row and a test; checking a parent category does
not check its children.

### 5.1 Command and resource semantics

- [ ] `SOSetTargets` records state and captures stream output.
- [ ] `DiscardResource` has D3D12-correct discard/undefined-content semantics.
- [ ] `SetPredication` controls every applicable command.
- [ ] Command-list markers/events and queue markers/events reach the provider.
- [ ] `AtomicCopyBufferUINT` is atomic and honors dependent ranges.
- [ ] `AtomicCopyBufferUINT64` is atomic and honors dependent ranges.
- [ ] `SetSamplePositions` affects raster/MSAA behavior.
- [ ] `SetViewInstanceMask` affects view-instanced output.
- [ ] Protected command-list association is stored and enforced.
- [ ] Meta-command initialization/execution has real providers.
- [ ] `Unmap` flushes/commits the correct CPU writes.
- [ ] Default/private `ReadFromSubresource` and `WriteToSubresource` work.
- [ ] `MakeResident`, `Evict`, priority, trim, and residency queries track
      actual state.
- [ ] Device removed reason and DRED reflect actual faults.
- [ ] Clock calibration returns correlated CPU/GPU timestamps.

### 5.2 Feature-query completion

- [ ] ROV support is backed by ordered UAV readback.
- [ ] Double precision is backed by arithmetic readback.
- [ ] Minimum precision is backed by conversion/rounding readback.
- [ ] Programmable sample positions are backed by sample coverage readback.
- [ ] View instancing and barycentrics are backed by per-view/primitive output.
- [ ] MSAA alignment/native16 reports match actual allocations/ALU behavior.
- [ ] Full RT-array and mesh/amplification derivative fields are tested.
- [ ] Mesh per-primitive VRS is tested and reported correctly.
- [ ] Options 13/15/16/17/19/20/21/22 fields are each implemented or the
      declared target is expanded to include their provider.
- [ ] Hardware copy, async commands, fence barriers, and barrier layouts have
      real behavior.
- [ ] Shader cache ABI, MLIR, linear algebra, SER, and byte-offset views have
      real behavior.
- [ ] Protected-resource support is backed by the security provider.

### 5.3 DXR completion

- [ ] Mixed geometry is native/semantically complete inside one BLAS.
- [ ] Every legal geometry count/layout/flag/update/transform is tested.
- [ ] Every shader stage and table kind is tested.
- [ ] All table strides/counts/offsets/local-root forms are tested.
- [ ] Any-hit/intersection/recursion/callable behavior is tested.
- [ ] State-object collections link independently and grow repeatedly.
- [ ] Serialized data rehydrates across processes.
- [ ] Source resources can be released at every documented point.
- [ ] Indirect dispatch validates nonzero offsets and all dimensions.
- [ ] Stack sizes and identifiers are deterministic and complete.

### 5.4 Mesh/work-graph completion

- [ ] All AS/MS payload sizes and thread-group shapes work.
- [ ] All stage resource classes and barriers work.
- [ ] Amplification shader patterns and derivatives work.
- [ ] Mesh per-primitive VRS works.
- [ ] Work-graph objects/properties/memory/records work.
- [ ] Node IDs, entrypoints, local roots, and graph indices work.
- [ ] Graph dispatch, fan-out, synchronization, overflow, and lifetime work.

### 5.5 Video/protected/DSR completion

- [ ] Every versioned video device and command-list interface is exposed.
- [ ] Decoder, encoder, processor, motion, and extension commands work.
- [ ] VideoToolbox/CoreVideo surfaces synchronize with D3D12 resources.
- [ ] Protected session security boundary is independently verified.
- [ ] Protected resource/heap/command/video association works.
- [ ] DSR factory/state/resource/scaling/presentation work.

### 5.6 DXGI/display completion

- [ ] CoreWindow and composition swapchains work.
- [ ] DXGI surfaces/subresource surfaces map and copy correctly.
- [ ] Display modes report actual formats and refresh rates.
- [ ] VBlank/ownership/gamma/frame statistics work.
- [ ] Desktop duplication and dirty-region metadata work.
- [ ] Overlay checks and composition have real behavior.
- [ ] Software/WARP adapter is a functioning CPU provider.
- [ ] Adapter/occlusion/stereo status callbacks are real and unregisterable.
- [ ] Shared resource adapter LUID and handles work across processes.

### 5.7 Agility/debug/cache completion

- [ ] Compiler factory and cache sessions work.
- [ ] Pipeline library/state-object database survives restart and corruption.
- [ ] Device factory/configuration/experimental APIs work.
- [ ] InfoQueue stores, filters, retrieves, and callbacks correctly.
- [ ] DRED, tools, instrumentation, manual write tracking, and trim callbacks
      work.
- [ ] All declared Device13–Device15 and later methods are implemented.

---

## 6. Validation commands and gate order

The exact commands will be extended as each phase lands, but the order is
fixed:

```sh
# 0. Contract and inventory
python3 tools/d3d12-metal-sdk/scripts/validate-full-surface-contract.py
python3 tools/d3d12-metal-sdk/scripts/validate-interface-census.py
python3 tools/d3d12-metal-sdk/scripts/check-noop-runtime-paths.py

# 1. Clean source build and ABI
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/prepare-dxmt-x86-llvm15.sh
python3 tools/d3d12-metal-sdk/scripts/check-winemetal-abi.py

# 2. Stage and inspect providers/runtime
python3 tools/d3d12-metal-sdk/scripts/stage-dxmt-runtime.py --profile full-surface
python3 tools/d3d12-metal-sdk/scripts/preflight-runtime-layout.py \
  --profile full-surface
python3 tools/d3d12-metal-sdk/scripts/validate-runtime-providers.py \
  --profile full-surface

# 3. Narrow phase gates, each from a fresh source-staged prefix
# (phase-specific switches are added with the implementation)
tools/d3d12-metal-sdk/scripts/run-source-probes.sh --full-surface

# 4. Aggregate authority
python3 tools/d3d12-metal-sdk/scripts/compare-contract.py \
  --profile full-surface
python3 tools/d3d12-metal-sdk/scripts/validate-full-surface-gate.py \
  --profile full-surface --results-dir tools/d3d12-metal-sdk/results

# 5. Legacy and live validation
python3 tools/d3d12-metal-sdk/scripts/validate-d3d10-d3d11-regression.py
python3 tools/d3d12-metal-sdk/scripts/run-runtime-doctor.py --profile full-surface
```

Every command that runs Wine must use the vendored MetalSharp Wine 11.5 binary,
matching `wineserver`, an isolated temporary prefix, and the source-staged
runtime. System/Homebrew Wine and persistent Steam prefixes are not evidence.

---

## 7. Completion contract

This roadmap is complete only when all of the following are true:

1. Every method in the full interface census has a real implementation and
   positive/negative contract row.
2. Every legal operation in the declared target executes; no legal operation
   is fail-closed.
3. Invalid inputs still fail with correct Windows-compatible validation.
4. No reachable empty method, placeholder output, dropped command, or
   `S_OK` no-op remains.
5. Feature queries are generated from passing behavior evidence.
6. The unsupported ledger contains no `unsupported` or
   `limited_to_proven_probe` entries for the declared target.
7. DXR Tier 1.1, SM6.7, VRS Tier 2, Mesh Tier 1, Tiled Resources Tier 3,
   Conservative Rasterization Tier 3, ROVs, writable MSAA, work graphs,
   video, protected resources, DSR, stream output, sharing, and all other
   declared surfaces have exact behavior evidence.
8. All 145 Agility-contract interfaces and their methods are accounted for.
9. D3D10/D3D11 regression coverage remains green.
10. PE/Unix/WOW64 ABI, exports, runtime hashes, provider selection, and
    staging layout pass.
11. Full isolated-prefix, differential, fuzz, lifetime, concurrency, and
    representative live-consumer validation passes.
12. Generated artifacts are absent from the commit, the branch is pushed, and
    the PR summary links the final full-surface evidence.

Until these conditions hold, the full-surface goal remains open regardless of
whether the scoped FL12_2 gate is green.

---

## 8. Risks and explicit decisions

| Risk | Required decision/mitigation |
| --- | --- |
| Metal lacks a direct primitive | Use a provider with exact semantics; do not report a query-only approximation |
| Protected memory may not be available on macOS/M4 | Treat as a hard feasibility gate; build a real trusted provider or change the host target |
| Work graphs have no native Metal scheduler | Implement a persistent compute/CPU scheduler and prove ordering/records |
| Video APIs are not Metal APIs | Use VideoToolbox/CoreVideo with explicit surface/resource synchronization |
| Cross-process Metal object transport differs from Windows handles | Use Mach/IOSurface/platform transport and test restart/lifetime/security |
| Full shader-model breadth is larger than the current corpus | Generate the opcode/intrinsic/stage matrix and require zero unhandled lowering |
| Current reports overstate limited implementations | Generate reports only from the full behavior ledger |
| Large scope causes stale artifacts or hidden regressions | Keep phase gates sequential, manifest-stage artifacts, and rerun ABI/hash gates |
| Real games hide unsupported paths | Use real games as additional coverage, never as a replacement for method-level probes |

---

## 9. Progress log

### 2026-08-27 — Expanded the target beyond scoped FL12_2 completion

- Recorded that the previous FL12_2/SM6.7/DXR PR proves a scoped surface, not
  the entire Agility 1.619.3 functional API.
- Converted the 14 current unsupported/limited ledger rows, explicit no-op
  methods, false-success paths, feature-query gaps, and DXGI/display stubs
  into this full-surface implementation backlog.
- Established the no-fail-closed acceptance rule: legal target operations need
  a native, emulated, CPU, OS-backed, or security-backed provider with exact
  behavior evidence.
