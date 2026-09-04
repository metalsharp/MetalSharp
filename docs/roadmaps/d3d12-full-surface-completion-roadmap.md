# D3D12 Full-Surface Completion Roadmap

**Created:** 2026-08-27  
**Status:** Active — Phase 0 complete; this roadmap supersedes the scoped
FL12_2/SM6.7/DXR completion claim for purposes of full API coverage.  
**Predecessor:** [D3D12 FL12_2 / SM6.7 / DXR completion roadmap](d3d12-fl12_2-sm67-dxr-completion-roadmap.md)  
**Target:** MetalSharp's vendored DXMT D3D12/DXGI/WineMetal runtime  
**Stable release baseline:** Microsoft DirectX Agility SDK 1.619.5
(`D3D12SDKVersion=619`, released 2026-07-30)

**Preview compatibility lane:** Agility SDK 1.721.3-preview
(`D3D12SDKVersion=721`, never a stable promotion gate)

**Reference contract:** `tools/d3d12-metal-sdk/contracts/agility-1.619.5-contract.json`

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

The target is the functional D3D12/DXGI surface represented by the latest
stable Agility 1.619.5 reference contract and the vendored Windows headers,
including:

1. Core D3D12 device, object, resource, heap, descriptor, fence, query,
   pipeline, queue, command-list, swapchain, and state-object interfaces.
2. Feature levels `11_0`, `11_1`, `12_0`, `12_1`, and `12_2`, with all feature
   reports backed by the behavior they advertise.
3. SM5.x DXBC/AIR and SM6.0 through SM6.9 DXIL paths, including every opcode,
   intrinsic, stage, resource dimension, legal view, and diagnostic in the
   stable shader-model target. SM6.10 preview behavior is tracked only in the
   separate preview lane below.
4. All D3D12 feature families currently represented in the SDK contract:
   resources, residency, sparse/tiled memory through Tier 4, barriers, VRS, MSAA, ROVs,
   conservative rasterization, view instancing, mesh/amplification shaders,
   work graphs, DXR 1.1 plus the stable 1.619 DXR 1.2 additions, video,
   protected resources, DSR, caches, and Agility extensions.
5. DXGI 1.6 factory, adapter, output, resource, surface, display, sharing,
   event, overlay, duplication, and presentation behavior.
6. Agility configuration, compiler/cache, pipeline/state-object cache, debug,
   DRED, tools, lifetime, sharing-contract, and newer interface methods in the
   1.619.5 contract.
7. ABI parity across x86_64 PE, x86_64 Unix, WOW64 call paths, Wine loader
   routing, and all matching Winemetal exports.

The 145 interfaces listed by `agility-1.619.5-contract.json` are an inventory
input, not proof that the current implementation already exposes all of them.
The first phase produces a method-level inventory and resolves every missing
method explicitly. The 1.619.5 package also publishes `D3D12Events.h` and adds
the current OMM-descriptor and tight-alignment constants; those inputs are
part of the stable contract even though the 1.619.3 interface method count is
unchanged.

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

### 1.3 SDK compatibility policy

Using the newest SDK as the only target would not maximize game
compatibility. Agility is side-by-side: an application may export a
`D3D12SDKVersion` and select an app-local `D3D12Core.dll`, while another game
uses the inbox runtime or an older Agility release. The runtime therefore uses
1.619.5 as the **development and stable-surface superset**, but it must keep a
version-aware compatibility profile for older callers.

The compatibility matrix is:

| Caller/runtime family | SDK version value | Roadmap treatment |
| --- | ---: | --- |
| No Agility / inbox D3D12 | OS-selected | Required legacy regression lane; preserve the inbox-compatible ABI and feature behavior |
| Agility 1.4.9–1.4.10 | `4` | Required legacy lane; DirectX 12 Ultimate/SM6.6-era behavior |
| Agility 1.600.x | `600` | Required legacy lane; test representative `.10` patch where applicable |
| Agility 1.602.x–1.611.x | `602`–`611` | Required compatibility lane; test each public version family and its changed validation/features |
| Agility 1.613.x–1.616.x | `613`–`616` | Required compatibility lane; includes retail Work Graphs/SM6.8, video, OMM, and tight-alignment history |
| Agility 1.618.x | `618` | Required compatibility lane; includes Advanced Shader Delivery and related cache/state-database behavior |
| **Agility 1.619.5 stable** | **`619`** | **Authoritative release contract and full-surface promotion gate** |
| Agility 1.721.3-preview | `721` | Separate opt-in preview gate; never used to make the stable 1.619.5 claim pass |

The matrix does not require shipping every historical patch DLL. It requires
testing representative callers from each ABI/semantic family and the exact
latest stable package. A patch release with the same numeric
`D3D12SDKVersion` still gets a pinned package hash and header/runtime
provenance, because its validation and bug-fix behavior can matter.

Compatibility rules:

- Do not overwrite or mix an application's Agility `D3D12Core.dll` and
  `D3D12SDKLayers.dll` from different SDK families. The SDK version exported by
  the process must match the selected package according to Microsoft's loader
  contract.
- Do not replace older callers with preview-only interfaces. Expose the
  versioned superset through the translation layer and preserve old vtable,
  validation, layout, and error semantics.
- Normalize feature reports by both caller SDK family and actual host/provider
  capability. A newer SDK does not make an unsupported Metal operation
  supported, and an older caller must not observe a newer preview field as a
  false success.
- Add a loader probe for inbox/no-Agility, every representative stable family,
  stable 619, and preview 721. Record selected SDK version, path, package
  hashes, interface census, feature reports, and behavior readbacks.
- A stable promotion requires the 1.619.5 matrix plus all older compatibility
  lanes. A preview promotion, if ever requested, is a separate release with
  its own contract, binary hashes, and explicit opt-in.

The stable target follows Microsoft's official release channel, not a moving
"latest" URL. When Microsoft publishes a newer stable package, add it as a
new pinned contract and rerun the full matrix before changing the stable
baseline. Preview packages may be evaluated in parallel but never silently
become the production target.

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
| Stream output | One/two non-rasterized DXBC vertex streams with independent counters and bounded overflow rejection | Declarations, all legal strides/streams, overflow continuation, DXIL/geometry capture, capture buffers, and downstream consumption |
| Sparse/reserved resources | Focused native sparse textures/buffers pass | All legal dimensions, formats, packed/partial mips, physical page ownership, mapping copies, queue ordering, residency, aliases, and `CopyTiles` |
| Shared resources | Pointer-free named/unnamed buffer, heap, fence, and one committed RGBA8 texture provider; broader formats/placements remain limited | Portable cross-process resource/heap/event transport for every legal resource shape, names, LUIDs, synchronization, lifetime, and security validation |
| Geometry shaders | Narrow SM5 vertex/geometry proof | General topology, resource, stream, primitive, state, and multi-stage coverage |
| Hull/domain shaders | One native tessellation proof shape | All patch layouts, partitioning, topologies, factors, resources, indexing, state, and shader conversion paths |

### 2.2 Feature and query gaps

The runtime still returns false, zero, partial, or otherwise conservative values
for feature families that this roadmap must implement before reporting support:

- ROVs and ordered pixel UAV access.
- Minimum-precision shader behavior; binary64 arithmetic is now exposed through
  the exact typed emulation provider, while native Metal double ALU remains
  unavailable.
- Programmable sample positions.
- Broader view-instancing and barycentric interpolation variants; bounded
  Tier-1 view replay and default perspective barycentrics are now proven.
- 64-KB MSAA alignment; native 16-bit shader operations now have a bounded
  Metal-half provider and report.
- Full RT-array index range and derivatives in mesh/amplification stages.
- Mesh per-primitive VRS.
- Triangle fan, dynamic strip-cut, dynamic depth bias, and GPU-upload format
  and heap-flag breadth beyond the tested buffer/RGBA8 texture provider.
- Non-normalized samplers and manual write tracking.
- Tier 4 tiled resources, including the stable 1.619 sparse-resource additions.
- SM6.9 long vectors, required 16/64-bit wave operations, float16 specials,
  SER, and opacity micromaps.
- Options 13, 17, 20, and other zeroed extension fields.
- Work graphs, node shaders, `SampleCmp` gradient/bias, extended command info.
- Tight alignment, hardware copy, async commands, fence barriers, and barrier
  layouts.
- Shader-cache ABI, MLIR, linear algebra, shader execution reordering, and
  byte-offset views.
- Protected-resource support.

The current contract also needs reconciliation: `D3D12_OPTIONS.TiledResourcesTier`
is reported as Tier 3 while the unsupported ledger still says the broader Tier 3
surface is not proven, and stable 1.619.5 defines Tier 4. This roadmap requires
one authoritative state and a full Tier 1–4 matrix, not contradictory documents.

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
- `d3d12_command_queue.cpp`: queue timestamp/calibration paths must use the
  provider's GPU time domain, bounded completion, and a real CPU/GPU mapping;
  any queue event path that logs without applying the required GPU/debug
  operation remains a blocker.
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
- `contracts/d3d12-sdk-compatibility-matrix.json` — inbox, historical stable,
  current stable 1.619.5, and preview 1.721.3 version families.
- `contracts/d3d12-interface-census.json` — IIDs, vtable order, methods,
  header provenance, and implementation owner.
- `contracts/d3d12-no-op-policy.json` — forbidden empty bodies, forbidden
  `S_OK` no-op patterns, and allowed validation-only returns.
- `results/d3d12-full-surface-gate-<profile>.json` — aggregate evidence.
- `results/d3d12-full-surface-gate-<profile>.md` — human-readable summary.

The existing `feature-support-contract.json`, `unsupported-api-ledger.json`,
`risky-stub-ledger.json`, the historical
`agility-1.619.3-contract.json`, the stable
`agility-1.619.5-contract.json`, and the probe matrix remain inputs. The new
full-surface contract and compatibility matrix become the merge authority for
the expanded scope.

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

### 3.4 SDK-version proof requirements

Every compatibility-lane result must record all of the following rather than
only the SDK major number:

- Exact NuGet package version, `D3D12SDKVersion`, package URL, and SHA-256 for
  `D3D12Core.dll`, `d3d12SDKLayers.dll`, headers, and tools.
- Whether the process used inbox D3D12, app-local Agility, the stable 619
  provider, or preview 721.
- `D3D12SDKPath`/`D3D12SDKVersion` exports and the selected loader path.
- Interface census, vtable sizes, feature-query results, and behavior
  readbacks for that lane.
- Any intentional version-specific validation or compatibility behavior.

The stable 1.619.5 gate is authoritative for release. Historical lanes prove
that the superset implementation remains compatible; the preview lane proves
only opt-in preview behavior and cannot change the stable result.

---

## 4. Sequential implementation phases

The phases are deliberately ordered. Do not promote a later feature or widen
its report while an earlier provider, ABI, memory, or synchronization phase is
red.

### Phase status

- [x] Phase 0 — Full inventory, source census, provider map, and no-op scan
- [x] Phase 1 — Provider, synchronization, and capability architecture
- [x] Phase 2 — COM objects, interfaces, and lifecycle
- [x] Phase 3 — Resources, heaps, virtual memory, residency, and sharing
- [x] Phase 4 — Queues, commands, barriers, and indirect work
- [x] Phase 5 — Shader compiler and SM5.x–SM6.9 execution (280/280 required opcode rows observed; 0 open)
- [x] Phase 6 — Graphics stages, rasterization, ROVs, VRS, MSAA, and formats (exhaustive-feasible ordinary-graphics matrix closed; see [Phase 6 exhaustive-feasible completion roadmap](d3d12-full-surface-phase6-exhaustive-feasible-roadmap.md))
- [ ] Phase 7 — Mesh, amplification, work graphs, and node shaders (mesh/AS-MS payload proof; work-graph gate open)
- [ ] Phase 8 — DXR 1.0/1.1 and stable DXR 1.2 additions (inline RayQuery foundation; ray-generation/SER/OMM gate open)
- [ ] Phase 9 — D3D12 video provider
- [ ] Phase 10 — Protected resources and security-sensitive paths
- [ ] Phase 11 — DSR and advanced display scaling
- [ ] Phase 12 — DXGI factories, outputs, surfaces, presentation, and display
- [ ] Phase 13 — Agility, caches, diagnostics, tools, and configuration
- [ ] Phase 14 — Conservative reports and unsupported-ledger removal
- [ ] Phase 14A — 1.721.3-preview compatibility lane (optional)
- [ ] Phase 15 — Exhaustive validation, differential testing, and game coverage
- [ ] Phase 16 — Reproducible staging and release delivery

### Phase 0 — Freeze the full inventory and downgrade the completion claim **[COMPLETE]**

**Goal:** Make the expanded target measurable and prevent the scoped gate from
being mistaken for full completion.

**Work:**

- Generate the interface census from the stable 1.619.5 headers/contract and
  the version matrix, covering all 145 listed interfaces and every method.
- Diff stable 1.619.5 against the historical 1.619.3 contract and record the
  OMM-descriptor alignment, tight-alignment constants, `D3D12Events.h`, and
  validation-message changes instead of silently treating patch releases as
  identical.
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

**Phase 0 completion evidence:**

- Stable 1.619.5 inventory: **145 interfaces / 537 methods** with a primary
  source owner recorded for every method.
- Runtime static scan: **163 files / 1,153 findings**, including 58
  unsupported-return candidates, 77 empty bodies, 78 capability literals, and
  all success/placeholder-return candidates. Findings remain open work; none
  are silently suppressed or promoted.
- Source-tree digest is recorded in the census and validated by
  `validate-interface-census.py`.
- `validate-full-surface-contract.py`, `validate-contracts.py`,
  `validate-probe-matrix.py`, shell syntax checks, and JSON parsing pass.

**Updated files and connections:**

- `tools/d3d12-metal-sdk/scripts/generate-full-surface-inventory.py` — builds
  the synchronized phase-0 contracts and report from the stable interface
  contract plus runtime sources.
- `tools/d3d12-metal-sdk/scripts/check-noop-runtime-paths.py` — static gap
  scanner consumed by the inventory generator and future phase gates.
- `tools/d3d12-metal-sdk/scripts/validate-interface-census.py` — verifies all
  145/537 contract rows and current runtime source digest.
- `tools/d3d12-metal-sdk/scripts/validate-full-surface-contract.py` — validates
  phase-0 state, provider policy, matrix, census, and scanner synchronization.
- `tools/d3d12-metal-sdk/contracts/d3d12-{interface-census,no-op-policy,provider-contract,full-surface-contract,full-surface-matrix}.json`
  — authoritative phase-0 artifacts consumed by later implementation phases.
- `docs/roadmaps/d3d12-full-surface-phase0-inventory.md` — committed human
  report connecting static findings to the implementation backlog.

### Phase 1 — Build the provider, synchronization, and capability architecture **[COMPLETE]**

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

**Phase 1 completion evidence:**

- Clean cross-build passed **158/158** targets, including the host capability,
  provider-selection, timeline, and resource-state integration units.
- The provider/timeline architecture probe passed **12 selection cases** plus
  the timeline case with `no_silent_fallback=true`.
- Source-staged M4 `--caps-only`, `--queues-only`, and
  `--barriers-render-pass-only` probes passed; the host snapshot recorded
  Metal 3.2/4 capability inputs, shared events, MTL4 queue, native
  raytracing, and sample-count mask `0x16`.
- Temporary stage/runtime, prefix, and source-Wine clones were deleted after
  each run; no `drive_c` or Wine registry markers remain under `/tmp`.
- `check-winemetal-abi.py` passed against the disposable source-staged runtime
  with zero missing exports and zero failures.
- Phase proof: `docs/roadmaps/d3d12-full-surface-phase1-provider-proof.md`.

**Updated files and connections:**

- `vendor/dxmt/src/dxmt/dxmt_capabilities.hpp/.cpp` — host capability snapshot
  consumed by `dxmt_device.cpp` and D3D12 device logging.
- `vendor/dxmt/src/dxmt/dxmt_provider.hpp/.cpp` — explicit provider
  requirements/selection consumed by `Device` and `MTLD3D12Device`.
- `vendor/dxmt/src/dxmt/dxmt_timeline.hpp` — CPU/GPU timeline abstraction
  consumed by `dxmt_command_queue.hpp/.cpp` and existing queue event paths.
- `vendor/dxmt/src/d3d12/d3d12_resource_state.hpp` — encoder-independent
  state/layout tracker consumed by `d3d12_resource.hpp` and queue barrier
  replay.
- `vendor/dxmt/src/dxmt/meson.build` — compiles the new provider/capability
  units into every DXMT build.
- `tools/d3d12-metal-sdk/probes/probe_provider_architecture/` and
  `tools/d3d12-metal-sdk/scripts/run-provider-architecture-probe.sh` — native
  provider/timeline proof without committing binaries.
- `tools/d3d12-metal-sdk/contracts/d3d12-provider-contract.json` — records
  provider, timeline, host-capability, and later-phase ownership.
- `docs/roadmaps/d3d12-full-surface-phase1-provider-proof.md` — exact Phase 1
  commands and observed readbacks/logs.

### Phase 2 — Complete core COM objects, interface exposure, and lifecycle **[COMPLETE]**

**Goal:** Make the complete object model correct before adding more GPU work.

**Work:**

- Implement exact `QueryInterface` exposure and vtable compatibility for all
  declared D3D12/DXGI/Agility interfaces.
- Complete private-data byte copying, interface ownership, names, destruction
  notifications, child-object lifetime, and parent/device links.
- Implement InfoQueue storage/retrieval filters, message limits, breaks,
  mute state, callbacks, and exact buffer-size behavior.
- Preserve the DRED/device-removed and live-object hooks required by the core
  object lifetime model; complete diagnostic data, callbacks, and tooling in
  Phase 13.
- Implement lifetime owners/trackers and destruction callbacks.
- Implement device factory/configuration/experimental-feature interfaces and
  reject only malformed requests.
- Add COM ABI tests for every interface: QI, AddRef/Release, method dispatch,
  output initialization, and object destruction.

**Exit gate:**

- Interface census has no missing QI or vtable owner.
- Every object category survives create/use/release/recreate cycles.
- InfoQueue, lifetime, and private-data tests pass under x86_64 and WOW64;
  DRED/tool behavior remains owned by Phase 13.

**Phase 2 completion evidence:**

- Source-staged object contract probe passed with **13 object categories** and
  `info_queue_pass=true`, including repeated-QI identity, filters, stacks,
  counters, message readback, break settings, mute state, and invalid-input
  checks.
- Private-data behavior passed across device, queue, allocator, command list,
  fence, descriptor heap, heap, resource, query heap, command signature, root
  signature, pipeline library, and shader-cache objects.
- D3D10/D3D11 clear/copy/readback regression passed after the shared lifecycle
  changes.
- Clean source build passed **158/158** targets; disposable 18-artifact
  staging and Winemetal ABI verification passed with zero failures.
- Temporary prefixes, source-Wine clones, build products, probe caches, and
  temporary stage data were removed. No Wine prefix markers remain in `/tmp`.
- Phase proof: `docs/roadmaps/d3d12-full-surface-phase2-com-proof.md`.

**Updated files and connections:**

- `vendor/dxmt/src/d3d12/d3d12_device.cpp` — concrete InfoQueue storage,
  filtering, counters, break/mute state, singleton QI, and device-owner
  release.
- `vendor/dxmt/src/d3d12/d3d12_device.hpp` — InfoQueue owner/mutex fields.
- `vendor/dxmt/src/util/com/com_private_data.cpp` — validated private-data and
  interface deletion/allocation semantics shared by all COM objects.
- `tools/d3d12-metal-sdk/probes/probe_object_contracts/probe_object_contracts.cpp`
  — executable InfoQueue and null-interface lifecycle assertions.
- `docs/roadmaps/d3d12-full-surface-phase2-com-proof.md` — exact Phase 2
  evidence and cleanup record.

### Phase 3 — Finish resources, heaps, virtual memory, residency, and sharing **[COMPLETE]**

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
- Finish reserved resources and Tier 1–4 sparse behavior, including the stable
  1.619 Tier 4 additions:
  `GetResourceTiling`, standard and packed/partial mips, native 1D/1D-array,
  2D/3D/array/cube layouts, all supported formats, physical page ownership,
  update/copy tile
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

**Phase 3 resource checkpoint evidence:**
`docs/roadmaps/d3d12-full-surface-phase3-resource-proof.md` now includes
legal-shape creation/validation, placed-buffer aliasing, multi-plane and NV12
copy footprints, complete-mip normalization, packed sparse mip-tail reporting,
array-slice `CopyTileMappings` with a cross-slice two-tile readback, preserved
1D-array/cube-slice I/O, D24/D32 depth/stencil plane I/O plus queued stencil
clear, relaxed-castable-format readback, tight and small-resource alignment,
`CREATE_NOT_RESIDENT`/enqueued residency, descriptor/query-heap residency,
cross-queue sparse mapping (including all tile-range modes), a 66-format
behavior-backed sparse tile-shape/readback matrix, packed-tail array tiling and both-slice
packed-tail readback, cross-dimension zero-mip normalization, direct
texture/BC/volume I/O, mipped arrays/volumes and
MSAA arrays, D16 and typeless R24/R32 depth-stencil/plane formats,
provider-less R32G32B32 rejection with zero allocation, checked
allocation/footprint arithmetic, mixed-alignment sidebands,
nonzero-offset placed-texture I/O, committed/placed GPU-upload buffer and
RGBA8 texture I/O, a committed GPU-upload R8/R16/RGBA16 format matrix,
custom-equivalent GPU-upload placement, Options16 reporting,
unsupported heap/resource/video/raytracing/fence flag rejection, DXGI
resource aggregation,
pointer-free named/unnamed buffer/heap/fence mappings, one committed
RGBA8 texture Mach-port mapping, inherited unnamed buffer/heap/fence
cross-process read/write/signal/metadata, adapter-LUID
mismatch rejection, and OfferResources/Trim/Reclaim residency-state evidence.
The latest stable source-staged run passes the closed Phase 3 gate with a
108-case format/shape/subresource matrix, complete per-subresource footprints,
three aligned placed-buffer offsets,
66 behavior-backed advertised sparse formats, native 1D/2D-array/3D mapping,
cross-resource mapping copies, packed-tail and partial-mip copies, physical-page
ownership, and nonzero-Z boxed-volume I/O. The residency pressure arena
allocates and touches 512 MiB before `OfferResources`/`Trim`/`ReclaimResources`;
read-only access, lifetime, adapter-LUID, and named/unnamed cross-process
sharing checks pass for buffers, heaps, fences, and textures. Unsupported
provider combinations remain explicitly fail-closed.

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

**Phase 4 command milestone evidence:**
`docs/roadmaps/d3d12-full-surface-phase4-command-proof.md` records the
behavior-backed atomic-copy, discard, predication, GPU-only indirect argument,
root-constant, enhanced-barrier, command-annotation, and tier-1 programmable
sample-position checks. The fail-closed manifest is
`tools/d3d12-metal-sdk/contracts/phase4-command-coverage.json`; its command
coverage rows are now closed after positive direct/indirect DISPATCH_RAYS and
DISPATCH_MESH replay readbacks. The latest source-staged probe also verifies a
bounded single-stream DXBC vertex capture (`initial_filled_size=16`, final
`filled_size=144`, preserved prefix, two exact 4-vertex payloads with counter
accumulation plus a bounded overflow rejection) and a two-stream capture with
independent prefixes (`16`/`32` bytes), exact payloads, and final counters
`144`/`160`, GPU-only
indirect DRAW pixel readback (`[255,0,0,255]`), GPU-only indirect
CBV/SRV/UAV+DISPATCH readback (`[31,32,33,34]`), GPU-only indirect
VBV/IBV+DRAW_INDEXED pixel readback (`[255,0,0,255]`), nonzero indirect
argument offsets after count clamping, exact direct/indirect raygen UAV values
(`0x52415931`), exact direct/indirect mesh UAV values (`0x4d455348`) and 72
nonzero raster pixels per path, view-instance mask routing to exact array-layer
readbacks (`slice0=[255,0,0,255]`, `slice1=[0,255,0,255]`), and a
four-sample/four-pixel programmable-position MSAA resolve with exact per-pixel
readback (`pixel1=[255,0,0,255]`, all other tested pixels clear), plus explicit
command histograms/unknown-type accounting. The queue probe also verifies normal
queue creation, explicit bundle/video/global-realtime/timeout validation results,
clock calibration, and cross-queue event completion. Its fresh source-staged
queue-timing lane now checks a 1 GHz provider frequency, monotonic GPU/CPU
calibration pairs, normalized host-domain agreement, exact null-pointer
rejection, and a bounded five-second GPU completion wait. Stream output remains
a limited provider: overflow continuation, DXIL/geometry-stage capture, and
downstream consumption are not promoted; the one- and two-stream lanes now
include independent nonzero prefixes/counters and exact append accumulation.
The Phase 4 command coverage
manifest is closed and its focused gate passes; broader stream-output,
indirect-work, and feature-family matrices continue in the later phases rather
than being promoted by this phase.

### Phase 5 — Complete the shader compiler and SM5.x–SM6.9 execution surface

**Goal:** Replace the reduced synthetic corpus with complete declared shader
semantics.

**Work:**

- Define exact supported shader-model range and stage matrix for SM5.x DXBC/AIR
  and SM6.0–SM6.9 DXIL; keep D3DCompile/DXC provenance in every result.
  Track SM6.10 separately under the 1.721.3-preview lane.
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
- Finish advanced SM6.7–SM6.9 texture behavior across applicable stages and
  views:
  programmable offsets, raw gather, `SampleCmpLevel`, gradient/bias forms,
  mip selection, borders, arrays, depth, and typed/castable views.
- Add the stable 1.619 SM6.9 operations: long vectors, required 16-bit and
  64-bit shader/wave operations, 16-bit float specials, and the DXR 1.2 shader
  interfaces used by SER and opacity micromaps.
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

**Phase 5 shader checkpoint evidence:**
`docs/roadmaps/d3d12-full-surface-phase5-shader-proof.md` records the typed
DXIL rejection boundary, SM5.0 through SM6.9 compile/link progression, exact
SM6.7/6.8/6.9 semantic readbacks, WaveOps/QuadOps matrix, the source-staged
resource-metadata/42-case typed texture matrix, exact single-counter
append/consume readbacks with bounded negative cases, malformed-DXIL
negative evidence, the SM6.9 eight-component long-vector FDot/arithmetic
matrix, and the native one-triangle TLAS `TraceRayInline` readback plus
unsupported-ray-flag rejection. The fail-closed manifest is
`tools/d3d12-metal-sdk/contracts/phase5-shader-coverage.json`; the complete
numeric opcode inventory is
`tools/d3d12-metal-sdk/contracts/phase5-sm5-sm69-opcode-stage-resource-matrix.json`
and is checked by `validate-sm5-sm69-opcode-matrix.py`. Its exhaustive
opcode/stage/resource/cache/session row remains open, so
`D3D12_FEATURE_SHADER_MODEL` continues to report only the behavior-backed 6.7
ceiling; the core TempRegLoad/TempRegStore and MinPrecXRegLoad/Store rows,
38 inline-RayQuery opcodes, six SM6.5 mesh/amplification opcodes, one ViewID
opcode, two SM6.8 extended-command-information opcodes, and the InnerCoverage
opcode are now observed, including exact temporary-register,
candidate/committed state-accessor, transform, contribution, procedural,
AllocateRayQuery2, abort, ViewID-default/instancing, conservative-raster, and
native direct/indirect mesh matrices, including the SM5 DXBC geometry
stream-restart/primitive-ID readback (`geometry-system-v2`, 1,062 nonzero
pixels), 64-byte, 128-byte, and 256-byte amplification payloads plus a
64-thread mesh group. The mesh proof
uses an explicitly selected
host `libmetalirconverter` cache provider while retaining the
`METAL_SHADER_CONVERTER=/nonexistent` runtime setting; vector temporary
breadth, broader DXR accessors, and state-object breadth remain open. The
bounded GPU HitObject provider now covers the recorded 262–289 forms,
including scalar MakeMiss/miss-shader Invoke, permitted no-reorder
MaybeReorderThread continuation, triangle BuiltInTriangleIntersectionAttributes,
local-root constants, and exact ray state/accessors. A separate GPU-native
one-node kernel provider covers representative Work Graph opcodes 238–253
without adding a CPU scheduler; D3D12 Work Graph API execution remains
unsupported and is not promoted.

The exhaustive Phase 5 matrix now has 280 observed, 0 open, and 32
reserved/not-applicable rows. The bounded AttributeAtVertex provider captures
one validated float32 `nointerpolation` vertex output into a transient GPU
buffer and returns exact three-vertex readbacks for a triangle-list draw on
Apple M4/Metal 4; other signature/type, operand, indexed/strip, multi-instance,
geometry/tessellation, and depth-bounds combinations remain fail-closed. The
bounded CycleCounterLegacy provider accepts the debug-only single-read form,
materializes its undefined initial value as zero, and rejects meaningful
multiple-read/delta forms before PSO creation because Metal has no shader
clock. The core temporary-register and min-precision
register forms have exact compute-UAV evidence, the SM5 DXBC/AIR geometry
provider has an exact stream-restart/primitive-ID readback including the
compiled `EmitThenCutStream` form and a source-staged two-instance
`GSInstanceID` readback, and the native SM5 tessellation proofs exercise
output-control-point input loads, patch-constant stores/loads, and
`SV_DomainLocation` interpolation. The bounded `LoadPatchConstant` proof
consumes an exact 0.25 inside factor and returns center pixel `0xff000040`.
Broader patch-constant layouts, non-default geometry instance counts, DXIL
geometry-provider breadth, vector overloads, and broader stage matrices remain
open.

**Phase 5 closeout:** The literal shader-opcode gate is closed at 280/280
required rows with exact bounded positive behavior and fail-closed negative
coverage. The CycleCounterLegacy provider is intentionally limited to one read
whose initial value is undefined; meaningful intra-invocation deltas remain
rejected on Metal 4. The shader-opcode portion of the Phase 7 Work Graph/node
family now has an explicit GPU-native one-node provider, but D3D12 Work Graph
state objects and multi-node scheduling remain a later API provider gap.
Compilation or interface presence alone cannot close that gap.
The bounded Phase 8 HitObject provider now covers the recorded 262–289 forms,
with optional SER scheduling represented by a documented no-reorder
continuation provider rather than a CPU scheduler.
Each batch must regenerate the opcode corpus, pass the strict matrix plus
shader-engine audit, record exact readbacks/rejections, clean its disposable
prefix/cache, and land as a checkpoint. No row will be changed to observed
solely to improve the percentage.

**Pull-forward evidence recorded without closing later phases:** Phase 6 now
has a closed behavior-backed provider matrix: exact conservative-raster
`InnerCoverage`, ViewID instancing, programmable sample-position/
attribute-evaluation, writable-MSAA, typed texture/ROV, graphics-state,
format, topology, stencil, and dynamic-depth-bias matrices. Combinations not
covered by a provider remain explicit fail-closed boundaries. Phase 7 has
host-specific native mesh/amplification payload proofs for 64/128/256-byte
payloads and a 64-thread group, while D3D12 Work Graph state-object and
multi-node API execution remain open despite the closed bounded node-shader
opcode lane.
Phase 8 has exact inline RayQuery state/accessor/transform/contribution/
procedural/abort behavior, indexed R16 BLAS coverage, and a host-specific
native ray-generation system-value and any-hit control-flow matrix, while
full table breadth, SER, OMM, and portable serialization remain open.

### Phase 6 — Complete graphics stages, rasterization, ROVs, VRS, MSAA, and formats

**Status:** Closed for the declared ordinary-graphics surface. Every legal
combination in the exhaustive-feasible manifest has a composed native,
replay/lowering, or GPU-reference provider with exact fresh-prefix evidence;
mesh/amplification/work graphs, DXR, video, protected resources, DSR, and
presentation remain separate phases. The bounded provider contract remains the
historical regression checkpoint. The closure record is
[Phase 6 exhaustive-feasible completion roadmap](d3d12-full-surface-phase6-exhaustive-feasible-roadmap.md).

**Implemented provider matrix:**

- Vertex/pixel graphics, depth/stencil, blend, logic-op MRT replay, geometry
  emulation, native hull/domain tessellation, triangle-fan and dynamic
  strip-cut expansion, and GraphicsCommandList8/9 state all have exact
  readback probes.
- ROV semantics use Metal raster-order groups for raw/structured/typed
  buffers and typed 1D/1D-array/2D/2D-array/3D/MSAA resources, with D32/D24S8
  state coverage and exact non-pixel-stage validation. Independent-logic UAV/
  ROV stores use a guarded exactly-once replay provider.
- Conservative rasterization uses a GPU point-coverage reference provider
  covering list/strip/fan, native 2x/4x, clipping, winding, viewport/scissor,
  D32 depth, degenerate triangles, and `SV_InnerCoverage`; VRS, view
  instancing, barycentrics, programmable sample positions, writable MSAA,
  formats, native16, binary64 emulation, alpha-to-coverage, and dynamic depth
  bias are each tied to exact declared probes.
- Native target MSAA is advertised only for host counts 1/2/4; flattened
  writable/ROV 8x is a separate provider. Invalid descriptors return exact
  HRESULT/null-object pairs, while valid declared combinations are not
  rejected as unfinished.

**Exit gate:**

- The CPU reference and GPU/provider readbacks agree for every row declared in
  `phase6-graphics-coverage.json`; the fresh selected-runtime graphics,
  ROV/VRS/barycentric/MSAA/format, and D3D10/D3D11 regression probes pass.
- Feature queries are derived from those passing matrices, while every
  unverified combination remains unadvertised or rejects before execution.
- The exhaustive manifest, all-options fresh-prefix gate, ABI/provenance
  checks, and `--require-complete` validator report zero open legal classes
  and zero unclassified legal values. Geometry-stage breadth is intentionally
  owned by the separate Phase 7 geometry/tessellation work.

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

**Bounded checkpoint (not the exit gate):** The source-owned
`probe-workgraph-execution` now validates a two-node graph, executes three
single-node CPU-input records and three GPU-input records, and executes both
multi-node CPU/GPU input modes with exact entrypoint-routed payload readback.
It also checks both local-root argument table indices, proves nested CPU
records remain pointer-free after the caller mutates its arrays, verifies a
malformed multi-node stride leaves the backing memory unchanged, validates a
non-empty node-local-root table and rejects malformed table stride, checks
bounded backing-memory overflow without writes, and checks ordered back-to-back
dispatches after command-list reuse. The command path remains pointer-free and records `cpu_scheduler=false`; the
reference kernel also passes direct-queue and compute-queue submission. The
compute case uses one CPU-input record after host completion of direct work;
`cross_queue_dispatch_exact` does not prove GPU queue-to-queue synchronization. The
source-owned `node_records` and
three-entrypoint `node_multi` DXIL libraries lower at runtime into GPU-native
Metal compute kernels with exact backing-buffer readback, CPU/GPU single-node
and one-input multi-node coverage, entrypoint routing, a node-barrier witness,
and INCLUDE_ALL_AVAILABLE_NODES metadata materialization, explicit backing-memory
initialization, and a direct node-u0 resource binding; resource/record fan-out,
recursion, cross-queue synchronization, and general node shader conversion
remain open.
`D3D12_OPTIONS21.WorkGraphsTier` is therefore still not promoted.

**Exit gate:**

- `D3D12_OPTIONS21.WorkGraphsTier` and all mesh/amplification fields are
  behavior-derived.
- Work graph/node probes execute nontrivial graphs and read back exact record,
  payload, resource, and ordering results.
- No work-graph or node method remains an absent interface, empty command, or
  placeholder property.

### Phase 8 — Complete DXR 1.0/1.1 plus stable DXR 1.2 additions

**Goal:** Turn the current foundational DXR bridge into complete declared DXR
1.1 behavior and implement the stable 1.619 DXR 1.2 additions.

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
- Implement stable DXR 1.2 Shader Execution Reordering (SER), including its
  shader/compiler interfaces, dispatch/reorder behavior, synchronization,
  and deterministic fallback when native Metal ordering is unavailable.
- Implement opacity micromap (OMM) arrays, descriptors, build/update,
  compaction, serialization, alignment, intersection/any-hit behavior, and
  exact visibility readback. Include the stable 1.619 four-byte OMM descriptor
  alignment and 128-byte array alignment rules.
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
  synchronization, release/lifetime, SER, and OMM matrices.
- No DXR ledger row remains `limited_to_proven_probe`.
- `D3D12_OPTIONS5.RaytracingTier` is reported only from this full result.
- Stable DXR 1.2 fields are reported only from the 1.619.5 behavior gate;
  preview-only 1.721 features remain in the preview lane.

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

**Bounded checkpoint (not the exit gate):** `probe-video` now exposes the
stable video-device ABI, one behavior-backed feature query, decoder/decoder
heap/processor object creation, exact descriptor round trips, and null/unknown
validation. A separate CPU-reference provider executes RGBA8 nearest-neighbor
`VIDEO_PROCESS` scaling and clockwise-180 orientation, NV12-to-RGBA8 BT.601
conversion, RGBA8-to-NV12 limited-range conversion, and P010-to-RGBA8
conversion into independent array subresources through a submitted video queue
with exact readback. P016 output encoding, VideoToolbox codec execution,
profiles, references, metadata, broader video queues, and protected video
remain unimplemented and no codec capability is promoted.

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

**Bounded checkpoint (not the exit gate):** The source-staged DXGI probe now
covers `DuplicateOutput`/`AcquireNextFrame` with a GPU desktop resource, an
exact CPU `MapDesktopSurface`/`UnMapDesktopSurface` readback, full-frame
dirty-rectangle sizing, one-frame timeout behavior, and release ordering. It
also creates a composition swapchain and verifies its descriptor/backbuffer, and maps a
surface through a DIB-backed `GetDC`/`ReleaseDC` round trip. The swapchain mini
probe verifies background-color and matrix state round trips,
rotation, HDR metadata reset, and the CoreWindow null-output validation.
DisplayLink/IOSurface capture, cursor/move metadata, composition/CoreWindow ownership,
software/WARP adapters, and complete color/presentation behavior remain open.

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

**Bounded checkpoint (not the exit gate):** `probe-diagnostics` verifies
state-object statistics, a serialized device-tools allocation-state blob,
mutable DRED settings, and the DSR factory boundary with null-on-rejection.
The InfoQueue1 callback probe additionally verifies registration, storage-filter
suppression, ignore-filter delivery, and unregister cleanup; the manual-write
probe verifies Options17 reporting, TrackWrite, and exact upload-buffer
visibility; the Debug1–Debug6 probe verifies mutable debug-layer, GPU-validation,
auto-name, and legacy-barrier state. The full DRED data
outputs, pageable tools, cache corruption/restart matrix, and DSR execution
remain open; these interfaces are not promoted as complete from compatibility
objects alone.

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

### Phase 14A — Maintain the 1.721.3-preview compatibility lane (optional)

**Goal:** Keep the implementation ready for callers that deliberately opt in
to the newest preview without contaminating the stable release claim.

**Work:**

- Import the exact 1.721.3-preview package and create a separate
  `agility-1.721.3-preview-contract.json` with package/header/runtime hashes.
- Diff the preview contract against stable 1.619.5 and inventory every new or
  changed interface, structure, enum, validation rule, and feature query.
- Add preview-only probes for SM6.10, LinAlg `VectorAccumulate`, partial and
  generic programs, GUID texture layouts, depth UAVs, compute linear algebra,
  dump-file configuration, and preview state-object/work-graph behavior.
- Preserve separate loader/runtime artifacts and never use preview headers or
  debug layers in the stable 619 evidence.
- Mark each preview result as `preview`, `not_release`, and `opt_in`; a preview
  failure cannot make the stable gate green or red.

**Exit gate:**

- The preview lane has its own zero-unknown interface census and explicit
  pass/fail result.
- Preview-only features are never advertised to a stable 619 caller.
- If a release elects to support preview callers, the preview gate becomes a
  separate release blocker with its own versioned runtime and rollback plan.

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
- [x] Bounded texture `ClearUnorderedAccessView` handles RGBA8 full/rectangular
      views, including GPU-only resources.
- [x] Bounded RGBA8 `DiscardResource` rectangles zero only the requested
      texture region; broader compressed/planar/depth and undefined-content
      matrices remain open.
- [ ] `SetPredication` controls every applicable command.
- [x] Command-list markers/events and queue markers/events reach the provider.
- [ ] `AtomicCopyBufferUINT` is atomic and honors dependent ranges.
- [ ] `AtomicCopyBufferUINT64` is atomic and honors dependent ranges.
- [x] Bounded four-pixel/four-sample `SetSamplePositions` changes raster/MSAA
      coverage; broader sample patterns remain open.
- [x] Bounded `SetViewInstanceMask` routes the exact two-view output; broader
      masks/layouts remain open.
- [ ] Protected command-list association is stored and enforced.
- [ ] Meta-command initialization/execution has real providers.
- [x] `Unmap` flushes/commits the correct CPU writes; a bounded RGBA8
      upload-texture shadow provider also supports Map/Unmap exact readback.
- [x] Default/private `ReadFromSubresource` and `WriteToSubresource` work.
- [x] `MakeResident`, `Evict`, priority, trim, and residency queries track
      actual state.
- [x] Device removed reason and zero-fault DRED 1.0–1.2 output paths are
      behavior-backed; populated fault-chain data remains open.
- [x] Clock calibration returns correlated CPU/GPU timestamps.

### 5.2 Feature-query completion

- [x] Bounded ROV buffer/structured/typed-texture access is backed by ordered
      UAV readback; full public ROV breadth remains open.
- [x] Independent per-render-target logic operations are backed by exact
      two-target XOR/AND readback through target-specific pipeline variants.
- [x] Binary64 emulation is backed by arithmetic/IEEE-754 conversion readback;
      native Metal double ALU is not claimed.
- [ ] Minimum precision is backed by conversion/rounding readback.
- [x] Tier-1 programmable sample positions are backed by exact four-pixel
      sample-coverage/resolve readback; broader matrices remain open.
- [x] Bounded view instancing is backed by exact per-view array output and
      SetViewInstanceMask routing; default perspective barycentrics also have
      exact fragment readback, while other interpolation variants remain
      conservative.
- [x] Native16 reports match the exact typed Metal-half arithmetic/math
      readbacks; 64-KB MSAA alignment remains conservative.
- [ ] Full RT-array and mesh/amplification derivative fields are tested.
- [ ] Mesh per-primitive VRS is tested and reported correctly.
- [ ] Options 13/20/21/22 fields are each implemented or the declared target
      is expanded to include their provider. Options 15/16/19 are bounded
      providers; Options17 now reports manual-write tracking from its exact
      upload-buffer provider while non-normalized samplers remain false.
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
- [x] Bounded desktop duplication exposes GPU resources, exact dirty-rect
      sizing, CPU MapDesktopSurface readback, release ordering, and one-frame
      timeout behavior; cursor/move/DisplayLink breadth remains open.
- [ ] Overlay checks and composition have real behavior.
- [ ] Software/WARP adapter is a functioning CPU provider.
- [ ] Adapter/occlusion/stereo status callbacks are real and unregisterable.
- [x] Shared resource adapter LUID and handles work across processes.

### 5.7 Agility/debug/cache completion

- [ ] Compiler factory and cache sessions work.
- [ ] Pipeline library/state-object database survives restart and corruption.
- [ ] Device factory/configuration/experimental APIs work.
- [ ] InfoQueue stores, filters, retrieves, and callbacks correctly.
- [x] Bounded DRED settings/data, tools, manual write tracking, InfoQueue1,
      and trim-callback paths work; full instrumentation and populated fault
      data remain open.
- [ ] All declared Device13–Device15 and later methods are implemented.

---

## 6. Validation commands and gate order

The exact commands will be extended as each phase lands, but the order is
fixed:

```sh
# 0. Contract and inventory
tools/d3d12-metal-sdk/scripts/fetch-agility.sh --version 1.619.5
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
7. DXR Tier 1.1 plus stable DXR 1.2 additions, SM6.9, VRS Tier 2, Mesh Tier 1,
   Tiled Resources Tier 4, Conservative Rasterization Tier 3, ROVs, writable
   MSAA, work graphs, video, protected resources, DSR, stream output, sharing,
   and all other declared surfaces have exact behavior evidence.
8. All 145 Agility-contract interfaces and their methods are accounted for.
9. Inbox/no-Agility and historical stable SDK compatibility lanes remain
   green, with stable 1.619.5 as the authoritative release lane.
10. D3D10/D3D11 regression coverage remains green.
11. PE/Unix/WOW64 ABI, exports, runtime hashes, provider selection, and
    staging layout pass.
12. Full isolated-prefix, differential, fuzz, lifetime, concurrency, and
    representative live-consumer validation passes.
13. Generated artifacts are absent from the commit, the branch is pushed, and
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
| Full shader-model breadth is larger than the current corpus | Generate the opcode/intrinsic/stage matrix through stable SM6.9 and require zero unhandled lowering; isolate SM6.10 preview work |
| Games use different Agility SDK generations | Keep a version-aware superset runtime and test inbox, historical stable, current stable, and preview lanes separately |
| Stable and preview headers drift | Pin exact package/header hashes and never use preview headers or layers in the stable 619 gate |
| Current reports overstate limited implementations | Generate reports only from the full behavior ledger |
| Large scope causes stale artifacts or hidden regressions | Keep phase gates sequential, manifest-stage artifacts, and rerun ABI/hash gates |
| Real games hide unsupported paths | Use real games as additional coverage, never as a replacement for method-level probes |

---

## 9. Progress log

### 2026-08-27 — Expanded the target beyond scoped FL12_2 completion

- Recorded that the previous FL12_2/SM6.7/DXR PR proves a scoped surface, not
  the entire Agility 1.619.5 functional API.
- Converted the 14 current unsupported/limited ledger rows, explicit no-op
  methods, false-success paths, feature-query gaps, and DXGI/display stubs
  into this full-surface implementation backlog.
- Established the no-fail-closed acceptance rule: legal target operations need
  a native, emulated, CPU, OS-backed, or security-backed provider with exact
  behavior evidence.
- Selected stable Agility 1.619.5 (`D3D12SDKVersion=619`) as the release
  baseline, with older/no-Agility compatibility lanes and a separate
  non-promoted 1.721.3-preview lane.

### 2026-08-29 — Phase 3 resource checkpoint continuation

- Added checked allocation and footprint arithmetic, requested-subresource and
  tiling-window bounds, D3D12 texture dimension/layout/format validation, and
  invalid buffer-flag rejection.
- Added DXGI resource aggregation with Offer/Trim/Reclaim evidence, portable
  unnamed CPU-visible heap/fence mappings, native 3D sparse mapping-copy and
  physical-page ownership, GPU-upload/custom-equivalent buffer and texture
  placement, and nonzero-offset placed-texture/depth-plane/boxed-volume I/O.
- Closed the Phase 3 exit gate after the source-staged resource probe passed
  the 108-case format/shape/subresource matrix, aligned placed offsets, the
  66-format behavior-backed sparse matrix, 512-MiB pressure/reclaim cycle,
  read-only access/lifetime checks, named/unnamed cross-process sharing, and
  adapter-LUID validation. Resource, views, command-replay, legacy, and caps
  probes plus the staged PE/Unix ABI check pass; generated build products are
  removed.

### 2026-08-29 — Phase 4 view/sample provider continuation

- Added a private ABI-compatible view-instancing stream-subobject copy that
  retains locations, validates masking flags/counts, and expands direct draws
  into per-view array-layer replay with viewport/scissor selection. The
  source-staged probe reads back red and green from the two independently
  selected layers.
- Extended programmable sample-position records to one-, two-, and four-pixel
  patterns (up to 128 positions). Direct draw replay selects each pixel's
  sample subset in a scissored MSAA pass; a four-sample 2x2 pattern produces an
  exact resolved pixel readback proving the per-pixel selection.
- Closed the view-instancing, multi-pixel sample-position, queue
  priority/VBlank/callback, and complete indirect-argument rows in the Phase 4
  evidence manifest. The source-staged command probe now records direct and
  ExecuteIndirect DISPATCH_RAYS and DISPATCH_MESH commands with exact UAV and
  raster readbacks; broader provider breadth remains assigned to later phases.

### 2026-08-29 — Phase 5 shader compiler checkpoint

- Added exact source-staged SM6.7, SM6.8, and SM6.9 semantic readbacks,
  including a pinned `float16_t` conversion lane, and a 4x4 texture
  Load/SampleLevel/SampleGrad/SampleBias/GatherRed/GetDimensions matrix.
- Extended the pinned DXC compile/link corpus through `cs_6_9` and added a
  fail-closed `shader/unsupported_semantics` boundary so nonzero lowering
  counters cannot become successful placeholder PSOs.
- Added the Phase 5 shader proof, lowering-report audit, and fail-closed
  `phase5-shader-coverage.json` manifest. Focused semantic, WaveOps,
  diagnostic, binding-baseline, and inline-RayQuery core rows are closed;
  the exhaustive SM5.x–SM6.9 opcode/stage/resource/cache/session row remains
  open.

### 2026-08-31 — Phase 5 native inline-RayQuery proof

- Added a direct Metal `intersection_query` lowering and acceleration-structure
  buffer binding path for the compute-stage DXIL RayQuery subset. The isolated
  M4 run proves exact one-triangle TLAS hit readback and rejects a shader using
  an unsupported ray flag with `0x80004005`; the six corresponding opcode rows
  are now observed without claiming the broader DXR or Phase 5 exit gate.

### 2026-08-31 — Phase 5 RayQuery state-accessor expansion

- Extended the inline-RayQuery lowering to dispatch the DXIL state-scalar and
  state-vector opcodes for candidate/committed front-face, barycentric,
  distance, instance/geometry/primitive, object-ray, world-ray, flag, and
  procedural-state accessors, plus an explicit aborted query. The isolated M4
  profile now checks an exact 40-word candidate/committed readback, including
  committed object-ray values, the triangle candidate's procedural-non-opaque
  false result, and an aborted query's empty committed status.
- Promoted 24 additional RayQuery opcode rows from runtime module-report and
  exact-readback evidence; the exhaustive matrix is now 180 observed, 100 open,
  and 32 reserved/not-applicable. Broader procedural geometry, transforms,
  ray-generation/state-object paths, and the Phase 5 exit gate remain open.

### 2026-09-01 — Phase 5 RayQuery transform accessors

- Extended the inline-RayQuery lowering to read all four native Metal
  candidate/committed object-to-world and world-to-object `float3x4` matrices,
  preserving DXIL row/column order and exact signed-zero payloads. The isolated
  M4 profile now checks an exact 88-word matrix with an x=`0.25` instance
  transform and its inverse, alongside the existing hit, state, and abort
  values.
- Promoted four additional RayQuery opcode rows from runtime module-report and
  exact-readback evidence; the exhaustive matrix is now 184 observed, 96 open,
  and 32 reserved/not-applicable. Contribution indices, broader procedural
  geometry, ray-generation/state-object paths, and the Phase 5 exit gate remain
  open.

### 2026-09-01 — Phase 5 extended command-information proof

- Added native vertex-pull handling for DXIL `StartVertexLocation` and
  `StartInstanceLocation`, preserving D3D12's original draw arguments while
  retaining Metal's vertex-ID offset behavior. The isolated M4 profile issues
  `DrawInstanced(3, 1, 4, 7)` and verifies 72 exact raster pixels with center
  `0xff4080ff` and zero unexpected pixels.
- Promoted both SM6.8 extended-command-information opcode rows from the
  runtime module report and exact raster readback. The exhaustive matrix now
  has 186 observed, 94 open, and 32 reserved/not-applicable rows; the Phase 5
  exit gate remains open.

### 2026-09-01 — Phase 5 RayQuery contribution-index proof

- Bound the D3D12 TLAS instance-contribution table through the reserved direct
  compute slot 30 and lowered candidate/committed contribution accessors by
  their native instance indices. The isolated M4 RayQuery profile now records
  exact candidate and committed contribution values of `23` alongside the
  non-identity transform and 90-word accessor readback.
- Promoted both contribution-index opcode rows from the runtime module report
  and exact readback. The exhaustive matrix now has 188 observed, 92 open, and
  32 reserved/not-applicable rows; contribution/counter slot aliasing and the
  broader DXR paths remain open.

### 2026-09-01 — Phase 5 inline procedural-hit proof

- Extended the inline-RayQuery probe to build a two-instance TLAS containing a
  translated triangle and a procedural AABB. `CommitProceduralPrimitiveHit(2.0)`
  now executes through native Metal `intersection_query` and returns exact
  committed status `2`, instance index `1`, instance ID `11`, and ray distance
  `2.0`, while the existing 90-word state matrix remains exact.
- Promoted the procedural-commit opcode row from the runtime module report and
  exact readback. The exhaustive matrix now has 189 observed, 91 open, and 32
  reserved/not-applicable rows; broader procedural geometry, contribution-table
  aliasing, ray-generation/state-object paths, and the Phase 5 exit gate remain
  open.

### 2026-09-01 — Phase 5 AllocateRayQuery2 proof

- Compiled the inline-RayQuery accessor lane as SM6.9 with the nonzero
  `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS` construction flag, causing DXIL
  `AllocateRayQuery2` to be emitted and lowered to the same native Metal query
  allocation while preserving exact triangle/procedural readbacks.
- Promoted the `AllocateRayQuery2` opcode row from the runtime module report
  and exact 96-word readback. The exhaustive matrix now has 190 observed, 90
  open, and 32 reserved/not-applicable rows; broader opacity-micromap data,
  contribution-table aliasing, and the Phase 5 exit gate remain open.

### 2026-09-01 — Phase 5 default ViewID proof

- Lowered the vertex-stage `SV_ViewID` system value through the per-draw
  metadata slot and verified the default view's exact zero value in the
  16x16 start-draw raster lane. The same source-staged DXIL report contains
  ViewID opcode 138 with zero unsupported intrinsics/opcodes.
- Promoted the default-view ViewID opcode row. The exhaustive matrix now has
  191 observed, 89 open, and 32 reserved/not-applicable rows; per-view replay
  and broader graphics-stage ViewID combinations remain open.

### 2026-09-01 — Phase 5 ViewID instancing proof

- Added a two-view native view-instancing replay using masks `0x1` and `0x2`.
  The vertex-stage `SV_ViewID` values 0 and 1 select exact red and green
  array-layer readbacks (`[255,0,0,255]` and `[0,255,0,255]`) on the Apple M4.
- The ViewID row now has exact per-view behavior evidence; broader view counts,
  viewport/render-target permutations, non-vertex stages, and mesh/tessellation/
  VRS/MSAA interactions remain open.

### 2026-09-01 — Phase 5 InnerCoverage proof

- Added a conservative-raster reference-provider four-corner test for
  `SV_InnerCoverage`. The isolated M4 profile renders the pinned 64x64
  triangle and reads back exactly 1,200 fully covered pixels, 204 edge-only
  pixels, zero unexpected values, and a white center pixel.
- Promoted the `InnerCoverage` opcode row from the runtime module report and
  exact conservative-raster readback. The exhaustive matrix now has 202
  observed, 78 open, and 32 reserved/not-applicable rows; broader mesh
  payload/output/resource/barrier/VRS matrices, conservative-raster rules, and
  the Phase 5 exit gate remain open.

### 2026-09-01 — Phase 5 temporary-register proof

- Added a source-owned DXIL-part fixture and bounded per-invocation typed
  temporary storage for `TempRegLoad`, `TempRegStore`, `MinPrecXRegLoad`, and
  `MinPrecXRegStore`.
- The `phase5-tempreg-overloads` compute probe stores `4660`, reloads it,
  adds one, and reads back exactly `4661`; it also verifies float, bool, and
  half `TempReg` overloads plus `5.0` through the min-precision
  pointer/component path, reading back exact `6.0` bits (`1086324736`) through a UAV with
  `METAL_SHADER_CONVERTER=/nonexistent`. All four core temporary-register rows
  are now observed; vector overloads, dynamic indexable min-precision
  addressing, and broader stage matrices remain open.

### 2026-09-01 — Phase 5 indexed BLAS proof

- Extended the native inline-RayQuery probe to use an R16 indexed triangle
  BLAS rather than only a non-indexed vertex stream. The `phase5-dxr-indexed`
  profile preserved the exact 96-word candidate/committed/accessor matrix and
  the `768/256` BLAS plus `1280/256` TLAS size readbacks.

### 2026-09-01 — Phase 5 mesh/amplification opcode proof

- Added an explicitly selected `libmetalirconverter` host provider for native
  mesh/amplification cache materialization; the runtime probe continues to set
  `METAL_SHADER_CONVERTER=/nonexistent`.
- The `phase7-mesh-payload64` profile executes direct and GPU-only indirect
  `DispatchMesh` with a 64-byte amplification payload, exact
  two-layer/depth/blend/wireframe readbacks, payload/UAV lane values, and
  `PIPELINE_STATISTICS1` (`AS=2`, `MS=2`, primitives=2).
  DXIL reports contain opcodes 168–173 with zero unsupported semantics. The
  broader mesh/work-graph matrix remains open and the host-specific provider
  does not change the fail-closed compiler-object boundary.

### 2026-09-01 — Phase 5 native ray-generation system-value proof

- Fixed the explicit `METALSHARP_NATIVE_IRCONVERTER=1` DXR preparation lane so
  it materializes ray-generation, dispatch-wrapper, miss, hit, callable, and
  procedural libraries even when `METAL_SHADER_CONVERTER=/nonexistent`; the
  generic portable converter remains disabled.
- Profile `phase5-closeout-dxr9` passes exact BLAS/TLAS construction,
  collection filtering/merge, direct and indirect `DispatchRays`, recursive
  miss/closest-hit/callable/procedural dispatch, local-root records, and a
  29-word system-value matrix: dimensions `[5,1,1]`, triangle
  `InstanceID=7`, `InstanceIndex=0`, `HitKind=0xfe`, `RayFlags=0`, zero
  primitive/geometry indices, `RayTMin=0`, `RayTCurrent=2`, identity
  world/object vectors and transforms. Follow-up profile
  `phase5-closeout-dxr-control4` executes `IgnoreHit` and
  `AcceptHitAndEndSearch` with exact miss/accepted-payload readbacks. The
  matrix now reports 227 observed, 53 open, and 32 reserved/not-applicable
  required rows; remaining table breadth, DXIL geometry breadth, Work Graph/SER, OMM, and portable
  DXR object-code coverage remain open.

### 2026-09-01 — Phase 5 SM5 geometry stream/system readback

- Added `geometry-system-v2`, an isolated Apple M4 execution of the SM5
  DXBC/AIR geometry provider with two emitted strips, `RestartStrip`,
  `SV_PrimitiveID`, and a source-staged `dcl_gsinstances 2` declaration. It
  requires exactly 1,062 nonzero pixels and left/right RGBA samples
  `[255,0,128,255]` and `[0,255,128,255]`, proving both geometry-instance
  values reach the output.
- Hardened the DXBC geometry signature parser for the
  `INPUT_GS_INSTANCE_ID` declaration and `INSTANCE_ID` system-value form; the
  compiled probe now records `gs_instance_id_matrix_verified=true`.
- Promoted the behavior-backed `EmitStream`, `CutStream`, `EmitThenCutStream`,
  two-instance `GSInstanceID` input, and first primitive `PrimitiveID` rows.
  `LoadPatchConstant` and the DXIL geometry-provider ABI remain open.
  `RestartStrip` is compiled by the pinned D3DCompile lane as the combined
  DXBC stream operation, and the result records
  `emit_then_cut_stream_verified=true`.

### 2026-09-01 — Phase 5 tessellation system-value checkpoint

- Re-ran `tessellation_shader_pso` after the geometry checkpoint with the
  pinned runtime and `METAL_SHADER_CONVERTER=/nonexistent`; it matched exactly
  1,352 nonzero pixels and center pixel `0xff407e81`.
- The proof exercises `SV_OutputControlPointID`/input-patch loads,
  `SV_TessFactor`/`SV_InsideTessFactor` stores, and `SV_DomainLocation`
  interpolation. Four corresponding rows were promoted:
  `LoadOutputControlPoint`, `DomainLocation`, `StorePatchConstant`, and
  `OutputControlPointID`.

### 2026-09-01 — Phase 5 bounded patch-constant load proof

- Added the explicitly bounded `tessellation_patch_constant` profile. It
  consumes a source-staged `0.25` `SV_InsideTessFactor` value in the domain
  shader and requires exactly 1,352 nonzero pixels with center pixel
  `0xff000040`.
- Extended the native tessellation proof provider with a separate exact
  shader-bytecode profile. The generic HS/DS path remains fail-closed; only the
  readback-backed proof shape promotes `LoadPatchConstant`.
- The opcode matrix now reports **234 observed / 46 open / 32 reserved**.

### 2026-09-01 — Phase 5 strict closeout checkpoint

- Re-ran the contract, full-surface, interface-census, shader-engine, and
  opcode-matrix validators after cleaning the failed AttributeAtVertex
  experiment. The opcode validator reports `312` total values, `280` required
  rows, `234` observed rows, `46` open rows, and `32` reserved/not-applicable
  rows.
- The 46 open required rows are explicitly retained as four blocker families:
  `109 CycleCounterLegacy`, `137 AttributeAtVertex`, Work Graph/node
  operations `238–253`, and SER/HitObject operations `262–289`. The matrix
  and Phase 5 coverage manifest remain open; no row was reclassified solely to
  improve the count.
- The failed Metal 4 `vertex_value<T>` AttributeAtVertex path returned no
  behavior-backed readback and its source changes, rebuilt binaries, caches,
  prefixes, and logs were reverted or removed. The tracked native
  `tools/d3d12-metal-sdk/scripts/probe-metal-vertex-value.mm` now records the
  exact negative boundary: Apple M4/Apple9 reports `supports_apple10=false`,
  the constant fragment control returns `[255,0,0,255]`, and the
  `vertex_value<T>` path completes with `[0,0,0,0]`. This establishes no
  positive opcode evidence and does not close row 137. Phase 5 remains
  unchecked and `D3D12_FEATURE_SHADER_MODEL` remains capped at the
  behavior-backed 6.7 value. Later Work Graph/node and SER/HitObject
  implementation is deferred until the two Phase-5-local rows have exact
  positive and negative evidence.

### 2026-09-01 — Phase 5 AttributeAtVertex hardware-boundary checkpoint

- Confirmed the DXIL ABI from the pinned DXC sources:
  `attributeAtVertex.<overload>(i32 137, i32 input-element-id, i32 row,
  i8 column, i8 vertex-index)`, with vertex indices restricted to `0..2` and
  the input signature required to be `nointerpolation`. The lowerer now emits
  stage-, operand-, and vertex-index-specific diagnostics and rejects the
  unsupported operation before MSL/PSO creation.
- Added the standalone native Metal 4 control probe
  `tools/d3d12-metal-sdk/scripts/probe-metal-vertex-value.mm`. Under Xcode 27
  beta 6 it compiles both functions and creates both PSOs, but on the Apple M4
  (Apple9) exact readback is `constant=[255,0,0,255]` and
  `vertex_value=[0,0,0,0]`; `MTLGPUFamilyApple10` is not reported. The probe
  exits successfully only for this explicitly identified negative boundary.
- Added `tools/d3d12-metal-sdk/scripts/probe-metal-cycle-counter.mm` as the
  matching native-provider negative. Metal 4.0 rejects `clock()` on Apple M4
  with the exact compiler diagnostic `use of undeclared identifier 'clock'`;
  this does not establish CycleCounterLegacy behavior.
- These are hardware capability/rejection proofs, not behavior-backed
  `AttributeAtVertex` or `CycleCounterLegacy` promotions. The opcode matrix
  remains **234 observed / 46 open / 32 reserved**, and Phase 5 remains open.

### 2026-09-01 — Phase 5 cycle-counter fail-closed hardening

- The legacy `DXILToMSL` fallback previously translated
  `cycleCounterLegacy` to a literal zero without incrementing its unsupported
  semantic count. It now records the same explicit rejection diagnostic as the
  typed lowerer and increments `unsupported_intrinsics`, so the D3D12 pipeline
  rejects the shader before MSL/PSO creation rather than silently producing a
  false value.
- Rebuilt `src/d3d12/d3d12.dll` from the pinned MetalSharp build with the
  change. An isolated ABI-matched staging of that rebuild passes the complete
  Winemetal export gate; the direct DXIL probe then reports
  `unsupported_intrinsics=1`, `unsupported_opcodes=0`, and
  `CreateComputePipelineState=0x80004005`. This is an exact fail-closed
  negative, not positive CycleCounterLegacy evidence; the row remains open
  pending an exact counter readback or a semantically equivalent provider.

### 2026-09-01 — Phase 5 AttributeAtVertex runtime rejection checkpoint

- Compiled a pinned DXC `ps_6_1` shader whose pixel DXIL contains twelve
  `attributeAtVertex.f32` calls (opcode 137) across three no-interpolation
  inputs and vertex indices `0..2`; its vertex companion compiled successfully.
- With the freshly rebuilt/staged ABI-matched runtime, device and root-signature
  creation returned `S_OK`, while graphics PSO creation returned exactly
  `0x80004005` after the DXIL report recorded `unsupported_intrinsics=12` and
  `unsupported_opcodes=0`. This confirms fail-closed runtime behavior without
  claiming a positive readback; row 137 remains open.

### 2026-09-01 — Phase 5 descending HitObject opcode-280 boundary

- Started the remaining-opcode pass at row 280 as requested. A minimal pinned
  DXC `lib_6_9` ray-generation shader emits `HitObject_MakeNop` (266),
  `HitObject_SetShaderTableIndex` (287), and three
  `HitObject_WorldToObject3x4` (280) matrix accesses.
- Both `DXILToMSL` and the typed `MSLLowering` now recognize the HitObject/SER
  declaration family, preserve the explicit DXIL opcode for diagnostics, and
  increment `unsupported_intrinsics` rather than silently emitting literal
  zero values. The direct converter checks report
  `unsupported_intrinsics=5`, `unsupported_opcodes=0`, with three explicit
  `opcode=280` rejection diagnostics. This is negative fail-closed evidence
  only; row 280 remains open until exact world-to-object readback or a
  semantically equivalent provider is implemented.

### 2026-09-01 — Phase 5 descending HitObject opcode-279 boundary

- Continued downward from row 280. A minimal pinned DXC `lib_6_9`
  ray-generation shader emits `HitObject_ObjectToWorld3x4` (opcode 279) in
  addition to the required HitObject construction operations.
- Both DXIL lowerers reject the shader with
  `unsupported_intrinsics=5`, `unsupported_opcodes=0`, including three
  explicit `opcode=279` diagnostics. Row 279 remains open: this hardening
  prevents a literal-zero fallback but supplies no positive transform
  readback or HitObject provider.

### 2026-09-01 — Phase 5 descending HitObject opcode-278 boundary

- Continued downward through `HitObject_ObjectRayDirection` (opcode 278) with
  a minimal pinned DXC `lib_6_9` ray-generation shader covering all three
  vector components.
- Both lowerers reject the shader with
  `unsupported_intrinsics=5`, `unsupported_opcodes=0`, including three
  explicit `opcode=278` diagnostics. Row 278 remains open because this is
  fail-closed negative evidence only and does not provide an object-ray
  direction readback.

### 2026-09-02 — Phase 5 descending HitObject GPU-provider checkpoint

- Added a bounded typed DXIL ray-generation provider for a one-invocation
  triangle `intersection_query`. It detects HitObject declarations from the
  parsed DXIL module (rather than relying on a raw byte-string search), emits
  the existing visible-function dispatch ABI, maps descriptor classes to the
  compact ray-generation table, and keeps the legacy converter fail-closed.
  The runtime tests use a freshly rebuilt matching Winemetal PE/Unix pair and
  `METAL_SHADER_CONVERTER=/nonexistent`; no binary, cache, prefix, or log is
  part of the repository change.
- The provider now executes `HitObject_TraceRay` (262), `FromRayQuery` (263),
  `MakeMiss` (265), `MakeNop` (266), `IsMiss`/`IsHit`/`IsNop` (269–271),
  `RayFlags`/`RayTMin`/`RayTCurrent` (272–274), world/object ray origin and
  direction (275–278), both 3x4 transforms (279–280), and scalar
  geometry/instance/primitive, hit-kind, and shader-table accessors (281–287).
  Exact readbacks include
  world origin `[0x40a00000,0,0xc0000000]`, object origin
  `[0,0,0xc0000000]`, object direction `[0,0,0x3f800000]`, object-to-world
  `[1.0,5.0,1.0]`, and world-to-object `[1.0,-5.0,1.0]`; constructor/state
  probes also return `MakeMiss: IsMiss=1, shader-table-index=3` and
  `MakeNop: IsNop=1`.
- The strict opcode validator now reports **258 observed / 22 open / 32
  reserved**. This is not Phase 5 closeout: `Invoke`/SER scheduling (267–268),
  local-root-table/attribute accessors (288–289), along with CycleCounterLegacy,
  AttributeAtVertex, and Work Graph/node rows, still require exact
  behavior-backed evidence.

### 2026-09-02 — Phase 5 HitObject local-root-table proof

- Added the tracked `probe_command_replay` local-root mode and its pinned
  `lib_6_9` shader fixture. A `MakeMiss` HitObject reads a constant from the
  miss shader-table local-root tail; the probe places `0xa1b2c3d4` at the
  record's first local-root DWORD and verifies exact direct and
  `ExecuteIndirect` readbacks (`2712847316` in both paths) under
  `METAL_SHADER_CONVERTER=/nonexistent`.
- Extended the typed ray-generation ABI with bounded shader-table record
  walking: the provider selects the miss table for miss objects, applies the
  16-bit miss-record index rule, skips the 32-byte shader identifier, bounds
  the requested four-byte root constant, and returns zero for NOP or
  unset-index objects. Opcode 288 is now observed; `HitObject_Attributes`
  289 and `HitObject_Invoke`/`MaybeReorderThread` 267–268 remain open.
- The strict opcode validator now reports **259 observed / 21 open / 32
  reserved**. Phase 5 remains open because CycleCounterLegacy, AttributeAtVertex,
  Work Graph/node operations, SER scheduling, and HitObject attributes still
  lack complete exact behavior evidence.

### 2026-09-02 — Phase 5 bounded HitObject_Invoke miss proof

- Added a tracked `probe_command_replay` Invoke mode and a pinned `lib_6_9`
  library containing one scalar 32-bit `[raypayload]`, `MakeMiss(0, 0, ray)`,
  and an exported `miss_shader` that writes `0x5678`.
- The typed GPU provider now uses the Metal visible-function table to invoke
  the linked miss function, with the converter's 48-byte metadata/payload
  wrapper and exact payload copy-in/copy-out. An ABI-matched runtime returned
  `direct_value=22136` and `indirect_value=22136` for direct and
  `ExecuteIndirect` DispatchRays with `METAL_SHADER_CONVERTER=/nonexistent`.
- The provider fails closed for hit invocation, nonzero miss-table indices,
  vector/packed payloads, miss resource/system-value bodies, and missing
  `miss_shader` exports. Opcode 267 is now observed only for this bounded
  provider; opcode 268 and 289 remain open. The strict validator is now
  **260 observed / 20 open / 32 reserved**.

### 2026-09-02 — Phase 5 HitObject attributes and no-reorder continuation

- Added tracked `probe_command_replay` modes for `HitObject_Attributes` and
  `MaybeReorderThread`. The attributes mode builds a one-triangle TLAS,
  traverses it through the typed GPU provider, and reads both
  `BuiltInTriangleIntersectionAttributes.barycentrics` words. Direct and
  `ExecuteIndirect` DispatchRays return exact `[0x3e800000, 0x3f000000]`
  (`0.25, 0.5`) with `METAL_SHADER_CONVERTER=/nonexistent`.
- The typed lowerer now preserves ABI-compatible scalar/vector attribute
  records for `FromRayQueryWithAttrs`, copies those records for
  `HitObject_Attributes`, and maps built-in triangle attributes to the
  committed Metal intersection-query barycentrics. Unsupported attribute
  layouts and sources remain fail-closed.
- `MaybeReorderThread` is implemented as its permitted no-reorder behavior:
  literal coherence hints are validated, the current GPU lane is preserved,
  and no CPU scheduler is introduced. Direct and `ExecuteIndirect` probes
  both return an exact continuation marker of `1`.
- Fixed compact LLVM 3.7 `EXTRACTELT` decoding so omitted vector type fields
  still consume an SSA result ID; this prevents later DXIL calls from being
  misidentified after vector attribute access. The strict validator is now
  **262 observed / 18 open / 32 reserved**; only CycleCounterLegacy,
  AttributeAtVertex, and Work Graph/node opcodes remain open in the matrix.

### 2026-09-02 — Phase 5 GPU-native Work Graph opcode batch (intermediate)

- Added source-owned SM6.8 node fixtures under
  `tools/d3d12-metal-sdk/probes/probe_workgraph/`, a pinned-DXIL node lowering
  helper, and a native Metal 4 probe with a bounded 15-second completion wait.
  The six fixture lanes exercise all sixteen required node opcodes `238–253`
  and return exact GPU readbacks for output/input handles, record pointers and
  completion, counts, sharing, barriers, indexed outputs, validity, and
  recursion depth.
- The provider is explicitly GPU-native and does not add a CPU scheduler. The
  aggregate proof reports `opcode_count=16`, `d3d12_work_graph_api_supported=false`,
  and `cpu_scheduler=false`; the separate API compatibility provider now
  exposes pointer-free graph properties, `SetProgram`, and a bounded
  three-record `DispatchGraph` path. Multi-node execution, graph state-object
  composition, backing-memory breadth, overflow, and feature promotion remain
  limited and fail-closed outside that proof.
- The strict opcode validator is now **278 observed / 2 open / 32 reserved**;
  only CycleCounterLegacy `109` and AttributeAtVertex `137` remain open. Phase 5
  itself remains open until those rows have exact positive or semantically
  equivalent runtime evidence.

### 2026-09-02 — Phase 5 bounded AttributeAtVertex GPU-capture proof (intermediate)

- Added a source-owned `ps_6_1`/`vs_6_0` fixture and the
  `probe_attribute_at_vertex` runtime lane. The fixture uses one
  `nointerpolation float4` input whose three vertex outputs are distinct
  (`0.125`, `0.5`, and `0.875`) and issues literal DXIL opcode-137 calls for
  vertex IDs 0, 1, and 2.
- The typed DXIL lowerer now emits a paired vertex/fragment capture ABI. The
  vertex stage writes the selected output to a transient GPU buffer at slot 28;
  the pixel stage reads the current primitive's three records using
  `primitive_id`. The D3D12 probe uses a bounded three-vertex triangle-list,
  one-instance draw and returns exact bit patterns
  `[0x3e000000, 0x3f000000, 0x3f600000]`.
- The provider is forced onto the typed lowering path rather than a cached
  converter object, includes the capture variant in shader cache identity, and
  rejects unsupported signatures/types, dynamic row/column/vertex operands,
  indexed/strip/multi-instance breadth, geometry/tessellation combinations,
  and depth-bounds slot conflicts. It does not depend on Apple-9
  `vertex_value<T>` support or introduce a CPU scheduler.
- The strict opcode validator is now **279 observed / 1 open / 32 reserved**;
  only CycleCounterLegacy `109` remains open at this intermediate checkpoint.

### 2026-09-02 — Phase 5 bounded CycleCounterLegacy undefined-initial proof

- Added pinned LLVM 15 source fixtures for one `CycleCounterLegacy` read and
  for a two-read intra-invocation delta. The typed lowerer materializes the
  single-read counter's undefined initial value as `uint2(0u)` and rejects
  multiple reads before MSL/PSO creation; the standalone legacy converter
  remains fail-closed.
- The tracked D3D12 probe creates a pixel PSO from the assembled single-read
  DXIL, executes a bounded triangle draw with a 15-second fence wait, and
  reads exact target bits `[0x3f800000, 0, 0, 0]`. The two-read shader is
  rejected with `0x80004005`, proving the meaningful-delta boundary remains
  fail-closed rather than pretending Metal has a shader clock.
- The native Metal 4 `clock()` rejection remains recorded as a hardware
  boundary. This provider is debug-only and does not advertise a real-time
  GPU cycle counter or alter the behavior-backed shader-model ceiling.
- The strict opcode validator is now **280 observed / 0 open / 32 reserved**;
  the required Phase 5 shader-opcode gate is closed and Phase 5 is marked
  complete. Broader provider bounds and later phases remain separately open.

### 2026-09-02 — Phase 6 bounded rasterizer-ordered UAV provider (intermediate)

- Added the source-owned `probe_rov` DXIL fixture and runtime lane. Pixel
  `RasterizerOrderedByteAddressBuffer`, `RasterizerOrderedStructuredBuffer`,
  and `RasterizerOrderedTexture2D<uint>` shaders perform ordered
  load/increment/stores for three overlapping primitives at one pixel and each
  return the exact UAV value `3` with `METAL_SHADER_CONVERTER=/nonexistent`.
  The v2 harness additionally compiles and wires an `R32_UINT`
  `RasterizerOrderedBuffer<uint>` fixture; its isolated runtime readback stays
  open until the paired-runtime PSO failure is resolved. The v2 output also
  records a pending two-render-target independent-logic rejection using the
  ROV shader; it is not counted until that negative result is observed.
- The DXIL resource metadata ROV bit is now carried through the direct binding
  plan. Marked pixel UAV buffers and textures receive Metal's
  `raster_order_group(0)` qualifier; non-pixel ROV uses remain fail-closed.
- This checkpoint does not promote `ROVsSupported`: the public report remains
  conservative until the complete ROV graphics state/format matrix has
  independent behavior evidence. VRS, conservative
  raster, geometry/tessellation, sample-position, formats, depth-bias, and
  broader MSAA work also remain open, so Phase 6 is not complete.

### 2026-09-02 — Phase 6 independent render-target logic-operation provider

- Removed the old valid-state rejection for differing
  `IndependentBlendEnable` logic operations. Metal's global logic-operation
  descriptor is now emulated with one target-write-mask pipeline variant per
  attachment, repeated draw encoding, depth/stencil state-only replay when
  needed, and restoration of the base pipeline. Pixel shaders with UAV side
  effects remain explicitly fail-closed rather than being executed repeatedly.
- `probe-graphics-pso` now executes an exact two-target matrix with a D32
  depth attachment and depth writes enabled: target 0 XOR returns
  `0xaaffff3c`, target 1 AND returns `0x550a0c30`. The formerly rejected
  `logic_op_mrt_independent_variants` case now creates successfully, and the
  depth/stencil state-only replay is exercised. The pipeline compiler now
  determines the independent-logic replay requirement before shader-side
  effect validation; the ROV v2 lane owns the corresponding rejection probe.
- This closes the independent per-render-target logic-operation row in the
  Phase 6 bounded coverage contract. Other Phase 6 graphics matrices remain
  open and no full Phase 6 promotion is claimed.

### 2026-09-02 — Phase 6 binary64 emulation report and readback checkpoint

- The exact 61-case DXIL semantic run passes all binary64 arithmetic,
  IEEE-754 edge, comparison, conversion, unary, predicate, and FMA cases.
  `D3D12_OPTIONS.DoublePrecisionFloatShaderOps` now follows the available
  typed binary64 emulation provider rather than claiming native Metal double
  instructions; native Metal double ALU remains unadvertised.
- `probe-device-caps` and the feature-level probe expose and validate the
  behavior-backed double-precision report. The same semantic gate backs
  `Options4.Native16BitShaderOpsSupported` through Metal half arithmetic while
  `MSAA64KBAlignedTextureSupported` remains false. Minimum precision remains
  conservative until its conversion/rounding matrix is independently closed.

### 2026-09-02 — Phase 6 bounded barycentrics provider

- Added the source-owned `probe_barycentrics` DXIL graphics fixture. The typed
  DXIL parser carries the default perspective `SV_Barycentrics` input-signature
  ID to the MSL fragment ABI's `barycentric_coord` builtin.
- On Apple M4/Metal 4, the one-pixel full-screen triangle returns exact RGBA8
  `[128,64,64,255]` (`[0.5,0.25,0.25,1]`), and the device/feature-level probes
  report `BarycentricsSupported=true`. The parser and MSL ABI now preserve
  noperspective, centroid, and sample interpolation modes from PSV0, but those
  variants remain unpromoted until they have independent exact readback.

### 2026-09-03 — Phase 6 conservative-raster coverage contract refresh

- Recorded the existing bounded conservative-raster reference-model proof in
  the Phase 6 contract: three clipped/partially covered triangles over an 8x8
  target, 105 exact red pixels, and `ConservativeRasterizationTier=3`.
- The contract now distinguishes that proven shape from the still-open
  top-left/degenerate/line/MSAA/VRS/array interactions and keeps the public
  Phase 6 status partial.

### 2026-09-03 — Fail-closed graphics and compute PSO creation boundary

- `CreateGraphicsPipelineState` and `CreateComputePipelineState` now return
  `E_FAIL` whenever compilation has terminally failed instead of returning a
  non-null PSO that later drops work during replay. The public creation paths
  now compile synchronously; only a successful provider compile produces a
  usable pipeline object. This protects the ROV, DXIL, and later shader probes
  from treating a deferred/failed provider result as successful behavior.

### 2026-09-03 — Phase 6 GraphicsCommandList8 stencil-reference proof

- Added the stable Agility 1.619.5 `ID3D12GraphicsCommandList8` ABI extension
  for `OMSetFrontAndBackStencilRef`, including command recording and a Metal
  front/back stencil-reference command. The graphics probe clears a one-pixel
  `D24_UNORM_S8_UINT` target to stencil `6`, sets references `5/7`, and draws
  front/back triangles with `LESS`/`GREATER` tests. Exact additive output is
  `[255,255,0,255]`, proving both reference values are applied rather than
  silently collapsing to one shared reference. Broader stencil/depth matrices
  remain part of the open Phase 6 gate.

### 2026-09-03 — Phase 6 selected-runtime refresh and logic-op proof repair

- Rebuilt the selected `dxmt_m12` runtime with pinned LLVM 15 and staged the
  matching PE/Unix Winemetal pair. The isolated runner copies every selected PE
  into its disposable prefix and registers a unique Unix-library alias.
- Corrected the default Wine override string to use explicit per-module
  `n,b` entries; the former comma-separated default could silently route
  D3D10/D3D11 probes through Wine's builtin renderer.
- Fixed the graphics probe's independent-logic UAV fixture to use `u2`, as
  required by the SM5 pixel compiler when two render-target outputs are
  present. The current graphics PSO matrix now passes exact target-0/target-1
  logic readbacks and the pre-replay UAV-side-effect rejection. Typed ROV,
  barycentric, writable-MSAA, VRS, and format probes also pass under the same
  staged runtime; broader Phase 6 matrices remain open.
- Added GraphicsCommandList9 replay for triangle fans, dynamic strip-cut
  restarts, and dynamic depth bias. Exact unindexed/indexed fan, restart, and
  equal-depth bias readbacks pass; Options15/16 now report those providers.

### 2026-09-03 — Phase 5 aggregate evidence-gate repair

- Repaired the Phase 5 aggregate validator so it honors manifest-specific
  result profiles, indexed result arrays, and exact integer/string/array/
  boolean assertions. The prior checker interpreted multi-element assertions
  only as nested boolean paths, so valid current evidence was reported as
  missing or false.
- Re-ran the required Phase 5 evidence with the selected matching PE/Unix
  runtime pair and `METAL_SHADER_CONVERTER=/nonexistent`: the repaired gate
  passes all 45 declared Phase 5 rows. The run also fixes the exact discard
  expectation to the mathematically correct 2,048 surviving pixels for a
  64x64 right half, records the 256-byte final mesh payload lane, and
  materializes the HitObject miss library through the selected native host
  converter when the external converter is intentionally absent.
- The Phase 5 closeout remains bounded: the public shader-model ceiling and
  all unsupported broader combinations stay fail-closed; this does not claim
  that the full-surface aggregate or later phases are complete.

### 2026-09-03 — Phase 5 dynamic core-opcode fixture correction

- Updated the core semantic fixture to load its one/zero, signed, unsigned,
  and bit-index operands from the runtime SRV instead of embedding constants.
  This prevents DXC constant folding from erasing intrinsic calls from the
  generated DXIL while preserving the exact 36-word readback matrix.
- Corrected the expected `FirstbitHi` result for `0x10` to `27`, matching the
  DXIL unsigned-high-bit definition; the refreshed core lane passes exact
  readback under the pinned no-offline-converter environment.

### 2026-09-03 — Phase 6 behavior-backed graphics provider closure

- Rebuilt and staged the selected `dxmt_m12` runtime with pinned LLVM 15,
  matching PE/Unix Winemetal artifacts, Metal 4, and
  `METAL_SHADER_CONVERTER=/nonexistent`. The fresh isolated D3D10/D3D11
  clear/copy/readback regression now passes both API generations exactly.
  Legacy probes use unique runtime aliases and the D3D10 core entrypoint so a
  stale same-named Wine builtin cannot masquerade as the selected build.
- Closed the ROV matrix at `exact=true`: raw, structured, typed buffers,
  typed 2D uint/float and 2D-array uint resources, D32 depth, D24S8 stencil,
  vertex/compute stage rejection, and independent-logic UAV rejection all
  pass. `ROVsSupported=true` is now derived from this matrix; unsupported
  resource combinations remain fail-closed.
- Generalized the conservative-raster reference provider with viewport,
  scissor, winding, depth, degenerate, and clipped cases. The six-case CPU/GPU
  comparison reads 176 exact red pixels. The graphics PSO matrix, writable
  MSAA 1x/2x/4x/8x paths, VRS matrix, barycentric readback, topology/strip-cut
  expansion, dynamic depth bias, front/back stencil references, and resource/
  view/format matrix all pass under the same isolated runtime.
- Updated the Phase 6 contract to `closed` with explicit provider scopes and
  fail-closed boundaries. This closes Phase 6's declared behavior-backed
  provider matrix; the new [exhaustive-feasible Phase 6 roadmap](d3d12-full-surface-phase6-exhaustive-feasible-roadmap.md)
  tracks remaining legal ordinary-graphics combinations before the final Phase
  6 claim. Work Graphs, full DXR, video, protected sessions, and other later
  full-surface ledger families remain separate phases.

### 2026-09-03 — Phase 6 exhaustive-feasible follow-up

- Added the exhaustive-feasible completion roadmap. It requires implementation
  or a proven host/toolchain no-go for broader interpolation, point/line and
  RasterizerDesc2 behavior, conservative-raster cross-products, MSAA/sample
  positions, view layouts, ROV breadth, and side-effect-safe logic-op replay.
- Reclassified the main roadmap's Phase 6 status as open for that expansion;
  the existing 14-row bounded contract remains closed as a regression gate.

### 2026-09-05 — DXR mixed-descriptor safety hardening

- Stopped the Apple Metal provider from constructing heterogeneous primitive
  acceleration-structure descriptor arrays that dispatch triangle-only
  selectors to AABB objects. The Winemetal bridge now rejects that unsafe
  combination so the D3D12 layer selects its explicit child-BLAS/TLAS
  compound provider. The current DXR acceleration-structure matrix passes
  without Objective-C exceptions; native heterogeneous BLAS remains a Phase 8
  open item.

### 2026-09-05 — Work Graph input, ordering, and overflow evidence

- Added a source-owned DXIL node-library path: the runtime extracts and
  lowers a node entrypoint with the existing DXIL-to-MSL lowering pipeline,
  registers it by the stable program identifier, and dispatches the resulting
  Metal compute kernel without a CPU scheduler. The D3D12 probe compiles the
  `node_records` fixture plus a three-entrypoint `node_multi` fixture,
  verifies exact backing-buffer writes, CPU/GPU single-node and one-input
  multi-node dispatch, entrypoint routing, INCLUDE_ALL_AVAILABLE_NODES metadata,
  backing-memory initialization, a direct node-u0 resource binding, direct and
  compute queue submission, and a node barrier, and keeps the existing
  pointer/overflow/ordering negatives. This is bounded node coverage, not
  general graph promotion.

- Extended the bounded GPU-native Work Graph proof with a valid node-local-root
  table, pointer-free caller-record ownership, ordered back-to-back dispatches
  after command-list reuse, malformed node-table/record-stride rejection, and
  backing-memory overflow rejection with unchanged output readback. The
  bounded provider remains non-promoted because recursive/fan-out graphs,
  barriers, and general node conversion remain open.

### 2026-09-05 — Video planar reference matrix

- Expanded the CPU-reference `VIDEO_PROCESS` provider with RGBA8 orientation,
  NV12-to-RGBA8 and RGBA8-to-NV12 limited-range BT.601 conversion,
  P010-to-RGBA8 and RGBA8-to-P010 conversion, and explicit P016 rejection.
  The source-staged queue probe verifies exact per-plane/per-subresource
  readback; VideoToolbox codec behavior remains unpromoted.

### 2026-09-04 — Bounded command, video, diagnostics, and display providers

- Added a real `VIDEO_PROCESS` command-list adapter with actual video-device/
  processor ABI exposure, CPU-reference nearest-neighbor RGBA8 scaling,
  clockwise-180 orientation, NV12-to-RGBA8 BT.601 conversion,
  RGBA8-to-NV12 limited-range conversion, P010-to-RGBA8 and RGBA8-to-P010
  conversion, array-subresource/plane output, queue submission, fence
  completion, and exact readback. VideoToolbox codec execution remains explicitly unpromoted.
- Added InfoQueue1 callback registration/filter delivery, the manual-write
  tracking resource interface with exact upload-buffer evidence, and the
  D3D12 sharing-contract provider with capturable-work bookkeeping, resource
  checksum, and shared-fence signaling.
- Added texture `DiscardResource`, rectangular UAV/RTV clear replay, and
  duplication `MapDesktopSurface`/one-frame timeout behavior, with exact
  isolated source-staged probes. Added CPU-visible RGBA8 upload-texture
  Map/Unmap, barrier-layout/closed-command-list validation, and bounded
  DRED 1.0–1.2 zero-fault outputs. Feature promotion and the full-surface
  gate remain disabled for all still-limited providers.
- Extended the diagnostic surface with Debug1–Debug6, debug-device,
  debug-command-queue/list resource-state assertions, InfoQueue1 callbacks,
  manual-write tracking, and the D3D12 sharing-contract fence/capture path;
  every bounded lane has a matching probe and exact result contract.
