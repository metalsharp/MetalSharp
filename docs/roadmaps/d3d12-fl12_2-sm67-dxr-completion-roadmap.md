# D3D12 Feature Level 12_2, Shader Model 6.7, and DXR Completion Roadmap

**Created:** 2026-08-25

**Status:** Active implementation plan

**Branch:** `feat/d3d12-fl12_2-sm67-dxr-completion`

**Target:** MetalSharp PR against `metalsharp/MetalSharp:main`

**Runtime under test:** MetalSharp Wine 11.5 (`~/.metalsharp/runtime/wine/bin/wine`)

**Toolchain under test:** Xcode 27 beta 6 build `27A5252f`, Metal toolchain `32023.921.5`
**Hardware proof host:** Apple M4, Apple GPU family 9, Metal 4, 16 GB unified memory

## 1. Objective and non-negotiable completion definition

This work completes the vendored DXMT D3D12 stack instead of merely changing
reported capability values. Completion requires executable behavior, not optimistic
`CheckFeatureSupport` responses.

The PR is complete only when all of the following are true:

1. `D3D12CreateDevice` correctly validates and supports feature levels `11_0`,
   `11_1`, `12_0`, `12_1`, and `12_2`.
2. `D3D12_FEATURE_FEATURE_LEVELS` reports `12_2` on the Metal 4 / Apple M4 proof
   host and returns the highest actually supported requested level.
3. Feature-level 12_2's required capabilities are implemented and individually
   proven, not inferred from a single successful device creation.
4. Shader Model 6.7 is supported end-to-end: DXC input, DXIL parsing, lowering,
   MSL compilation, pipeline creation, binding, execution, and readback.
5. DXR 1.1 is implemented end-to-end through Metal acceleration structures,
   state objects, shader identifiers/tables, acceleration-structure commands,
   and ray dispatch.
6. D3D12, DXGI, `dxgi_dxmt`, `winemetal.dll`, and `winemetal.so` expose a
   coherent ABI and behavior surface with no dangerous no-op success paths.
7. Feature reports match behavior. No capability is considered complete merely
   because a query returns `TRUE`, a tier, or a newer shader-model number.
8. Existing lower feature levels and the established D3D10/D3D11 routes do not
   regress.
9. Every runtime probe uses the vendored MetalSharp Wine 11.5 binary and its
   matching `wineserver`; system/Homebrew Wine is not used.
10. Every probe iteration uses an isolated temporary prefix. Evidence is copied
    out before the prefix is stopped and deleted.
11. Rebuilt artifacts are staged by a manifest-driven path, hashes are verified,
    and the intended `d3d12`/`dxgi`/`dxgi_dxmt`/WineMetal artifacts are proven to
    load.
12. The full contract, build, ABI, probe, feature-level, shader, DXR, and runtime
    gates pass before the PR is opened.
13. The branch is pushed and a PR is opened with the evidence matrix attached.

The phrase “supported” in this roadmap always means: implemented, exercised by a
focused probe, and included in a final clean-prefix gate.

## 2. Authoritative feature requirements

The implementation baseline follows Microsoft's public feature-level and shader
model specifications:

- Feature level 12_2:
  <https://microsoft.github.io/DirectX-Specs/d3d/D3D12_FeatureLevel12_2.html>
- Shader Model 6.7:
  <https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_7.html>
- SM 6.7 advanced texture operations:
  <https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_7_Advanced_Texture_Ops.html>
- Apple Metal feature-set tables:
  <https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf>
- Metal acceleration structures:
  <https://developer.apple.com/documentation/metal/ray-tracing-with-acceleration-structures>

Feature level 12_2 requires at least the following public capability posture:

| Capability | Required FL 12_2 value | Current DXMT result | Completion evidence |
| --- | --- | --- | --- |
| Shader model | At least 6.5 | 6.5 | SM 6.7 corpus and runtime readback |
| Ray tracing | Tier 1.1 | Not supported | DXR 1.0/1.1 probes |
| Variable-rate shading | Tier 2 | Not supported | Per-draw and image VRS probes |
| Mesh shaders | Tier 1 | Not supported | AS/MS compile, PSO, direct and indirect dispatch |
| Sampler feedback | Tier 0.9 | Not supported | Feedback UAV creation/write/resolve probe |
| Resource binding | Tier 3 | Reported tier 3 | Unbounded/direct indexing runtime probes |
| Tiled resources | Tier 3 | Reported unsupported; reserved resources incorrectly succeed as committed | Sparse mapping and residency probes |
| Conservative rasterization | Tier 3 | Tier 1 | Tier-3 edge/coverage behavior probe |
| Root signature | 1.1 | Reported 1.1 | Existing plus direct-indexing extension probes |
| Depth bounds | Supported | Unsupported/no-op | Depth-bounds render probe |
| WriteBufferImmediate | Direct, compute, bundle | Reported none; partial command exists | Queue-type and bundle execution probes |
| GPU VA bits/resource | At least 40 on x64 | 40 | Address-range and bounds probes |
| GPU VA bits/process | At least 40 on x64 | 40 | Address-range and bounds probes |
| Wave operations | Supported | Reported true, but baseline contract rejects the claim | Wave runtime readback suite |
| Output-merger logic op | Supported | Reported true | Logic-op render/readback matrix |
| VP/RT array index from rasterizer feeder | Supported | Reported true | VS/DS/GS/MS array-index probe |
| Copy-queue timestamps | Supported | Unsupported | Copy queue timestamp query probe |
| Fully typed format casting | Supported | Reported true | Castable-format creation and view probe |
| Unaligned block textures | Supported | Unsupported | BC resource footprint/copy/sample probe |
| Int64 shader ops | Supported | Reported true | Arithmetic and atomic runtime readback |

Shader Model 6.7 completion additionally includes:

- Raw gather.
- `SampleCmpLevel`.
- Programmable texture offsets.
- Writable MSAA textures.
- `QuadAny` and `QuadAll`.
- `WaveOpsIncludeHelperLanes` behavior.
- All earlier SM 6.0–6.6 requirements used by the existing synthetic corpus,
  including wave operations, resource descriptor heap indexing, 64-bit values,
  atomics, barriers, derivatives, helper lanes, and root-constant binding.

## 3. Phase 1 audit results

### 3.1 Baseline execution result

An isolated Wine 11.5 prefix was created, probed, stopped, and deleted. The
captured result is ignored build evidence at:

`tools/d3d12-metal-sdk/results/probe-device-caps-current-baseline.json`

Observed baseline:

- Device creation succeeds.
- Maximum reported feature level: `12_1`.
- Maximum reported shader model: `6_5`.
- DXR tier: unsupported.
- Mesh shader tier: unsupported.
- Sampler feedback tier: unsupported.
- Atomic64 typed/group-shared/descriptor-heap support: unsupported.
- Reserved-resource creation incorrectly succeeds using committed-resource
  compatibility behavior.
- Wave operations are reported as supported while the current contract says
  runtime correctness is not proven.
- Overall capability probe result: **fail**.

This is not a feature-report-only gap. The source audit found missing execution
paths corresponding to every advanced capability above.

### 3.2 Build and toolchain audit

The clean source tree did not initially build under Xcode 27 beta 6 and current
MinGW GCC 16:

1. `vendor/dxmt/src/util/com/com_guid.cpp` used `std::setfill`/`std::setw`
   without including `<iomanip>`.
2. `air_tessellation.metal` called the private
   `__metal_atomic_fetch_add_explicit` intrinsic with an obsolete signature.
3. The x86_64 `winemetal.so` link selected an unrelated arm64-only
   `/usr/local/lib/libz.a` instead of the selected Xcode SDK's universal stub.

The branch now contains narrow compatibility fixes for these three baseline
build failures. The full vendored artifact target set builds with:

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/prepare-dxmt-x86-llvm15.sh
```

These build fixes are prerequisites, not proof of D3D12 completeness.

### 3.3 D3D12 entry-point and device audit

Relevant files:

- `vendor/dxmt/src/d3d12/d3d12.cpp`
- `vendor/dxmt/src/d3d12/d3d12_device.hpp`
- `vendor/dxmt/src/d3d12/d3d12_device.cpp`

Findings:

- `D3D12CreateDevice` does not reject unsupported requested feature levels. It
  constructs a device regardless of `MinimumFeatureLevel` and even returns a
  successful support-probe result without validating the requested level.
- `CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS)` hard-caps at `12_1`.
- Shader model reporting hard-caps at 6.5 by default and has an environment
  escape hatch for 6.6, not a behavior-derived capability model.
- The default unhandled feature-query path zeros unknown structures and returns
  `S_OK`. That can incorrectly convert a missing implementation into a valid
  unsupported response and can hide ABI-size mistakes.
- Several values are over-reported relative to implementation, including ROVs,
  conservative rasterization tier 1, typed UAV additional formats, and some
  format atomic flags.
- Feature-level 12_2 requirements currently reported false include copy queue
  timestamps, depth bounds, tiled resources, VRS, mesh shaders, sampler
  feedback, enhanced barriers, and unaligned block textures.
- Shared handles and opening shared heaps are `E_NOTIMPL`.
- Protected-resource, lifetime-tracker, meta-command, and state-object paths
  contain `E_NOTIMPL` returns.
- `GetRaytracingAccelerationStructurePrebuildInfo` writes zeros.
- `CreateSamplerFeedbackUnorderedAccessView` is a no-op.
- Reserved resources are not sparse resources; they are silently created as
  ordinary resources.
- Later device interfaces are compatibility declarations rather than complete
  Agility interface implementations.
- Object private-data support is implemented only on the device; most child
  objects still return `E_NOTIMPL` from `GetPrivateData` and discard values
  passed to `SetPrivateData*`.

### 3.4 Command-list and replay audit

Relevant files:

- `vendor/dxmt/src/d3d12/d3d12_command_list.hpp`
- `vendor/dxmt/src/d3d12/d3d12_command_list.cpp`
- `vendor/dxmt/src/d3d12/d3d12_command_defs.hpp`
- `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp`

The command stream currently has records for the core draw, dispatch, copy,
barrier, root binding, render target, clear, query, and indirect operations.
However, these D3D12 command-list methods are empty or compatibility no-ops:

- `CopyTiles`
- `SOSetTargets`
- `DiscardResource`
- `SetPredication`
- marker/event commands
- `AtomicCopyBufferUINT`
- `AtomicCopyBufferUINT64`
- `OMSetDepthBounds`
- `SetSamplePositions`
- `SetViewInstanceMask`
- protected-resource sessions
- meta-command initialization/execution
- all acceleration-structure build/copy/postbuild commands
- `SetPipelineState1`
- `DispatchRays`
- `RSSetShadingRate`
- `RSSetShadingRateImage`
- `DispatchMesh`

The command enum has no records for those operations, so they cannot be fixed
only in queue replay. Recording, object retention, serialization, command-list
lifecycle validation, and replay all need to be implemented together.

Additional correctness issues:

- Command methods generally do not validate closed/open state.
- `Close` can be repeated and `Reset` does not fully validate allocator state.
- `ExecuteBundle` directly splices bytes and does not retain every object the
  copied records reference.
- Resource, descriptor heap, root signature, and query heap references embedded
  in command bytes are not uniformly retained for command-list lifetime.
- Several state setters are represented by the same `CmdSetRootCBV` layout,
  relying on the enum alone to distinguish CBV/SRV/UAV semantics.
- Feature reports deny enhanced barriers and barrier layouts; no command-list 7
  enhanced-barrier interface is exposed.

### 3.5 Resource, heap, descriptor, and residency audit

Relevant files:

- `d3d12_resource.*`
- `d3d12_heap.*`
- `d3d12_descriptor_heap.*`
- `d3d12_device.cpp`
- `d3d12_command_queue.cpp`

Findings:

- Resource creation supports ordinary buffers and 1D/2D/3D textures, arrays,
  mip levels, and MSAA shapes at a basic level.
- Unsupported DXGI formats can silently fall back to BGRA8 instead of failing.
- Texture resources receive synthetic GPU virtual addresses, even though D3D12
  GPU VAs are buffer-oriented. Address lookup therefore mixes real and
  synthetic ranges.
- `Map`/`WriteToSubresource`/`ReadFromSubresource` contain compatibility
  success paths that do not perform required texture transfers.
- `ReadFromSubresource` can zero output and return success for GPU-only
  textures rather than executing a readback.
- Placed texture resources do not implement true heap aliasing.
- Reserved resources are committed-resource substitutes.
- Tile mappings are logged but not executed.
- Residency calls mostly return success without enforcing or tracking the
  requested state.
- `GetCopyableFootprints` needs plane-aware and all-format validation.
- Descriptor creation and copies have existing coverage, but complete
  descriptor-heap indexing, null descriptors, counters, and acceleration
  structure SRVs require expanded contracts.
- Sampler feedback descriptors are absent.
- Shared resources/handles are absent.

### 3.6 Pipeline and shader audit

Relevant files:

- `d3d12_pipeline_state.*`
- `d3d12_shader_compiler.*`
- `vendor/dxmt/src/airconv/dxil/*`
- `vendor/dxmt/src/airconv/dxbc_*`

Findings:

- The current path has substantial DXBC and DXIL support and a useful synthetic
  corpus, but the contract honestly caps the reported model at 6.5.
- Wave operations are partially lowered, but the baseline feature query reports
  them before the current probe policy accepts runtime correctness.
- The source tracks unsupported intrinsic/opcode counts and rejects those
  shaders; this is useful diagnostics but not full SM6.7 coverage.
- Mesh and amplification shader kinds are recognized by metadata, but D3D12
  pipeline-state stream subobjects are currently read and ignored.
- `DispatchMesh` is empty.
- A Metal mesh pipeline exists for geometry-shader emulation and tessellation
  proof shapes. This is reusable infrastructure, not proof of D3D12 mesh shader
  support.
- General geometry shader support remains limited.
- Stream output is explicitly rejected.
- Native tessellation is restricted to a proof shape; unsupported shapes are
  rejected/skipped.
- Shader Model 6.7 advanced texture operations do not have complete lowering or
  probes.
- DXIL ray-query and raytracing intrinsics do not have a complete Metal lowering
  model.
- `GetCachedBlob` on pipeline states is `E_NOTIMPL` despite pipeline cache and
  binary archive infrastructure.

### 3.7 DXGI and `dxgi_dxmt` audit

Relevant files:

- `vendor/dxmt/src/dxgi/dxgi_factory.cpp`
- `vendor/dxmt/src/dxgi/dxgi_adapter.cpp`
- `vendor/dxmt/src/dxgi/dxgi_output.cpp`
- `vendor/dxmt/src/dxgi/dxgi_resource.hpp`
- `vendor/dxmt/src/d3d12/d3d12_dxgi_device.cpp`
- `vendor/dxmt/src/d3d12/d3d12_swapchain.cpp`

Findings:

- Factory interfaces through `IDXGIFactory7`, adapter enumeration, GPU
  preference, LUID lookup, outputs, and HWND swapchains have useful coverage.
- CoreWindow/composition swapchains are `E_NOTIMPL`.
- Shared-resource adapter LUID is `E_NOTIMPL`.
- Adapter content-protection and memory-budget notification methods use TODO
  assertions rather than valid COM behavior.
- Adapter-changed event registration is a risky stub.
- DXGI resource subresource surfaces are stubbed.
- D3D12 DXGI surface creation is `E_NOTIMPL`.
- `EnqueueSetEvent` fails.
- Offer/reclaim/trim and some priority/latency calls return success without a
  full state model.
- Several output duplication/overlay/gamma/ownership paths are incomplete.
- Swapchain color space, HDR metadata, transforms, frame latency, fullscreen,
  and resize need a focused conformance matrix across `IDXGISwapChain1`–`4`.

### 3.8 WineMetal bridge audit

Relevant files:

- `vendor/dxmt/src/winemetal/winemetal.h`
- `vendor/dxmt/src/winemetal/Metal.hpp`
- `vendor/dxmt/src/winemetal/winemetal_thunks.*`
- `vendor/dxmt/src/winemetal/unix/winemetal_unix.c`

Findings:

- The bridge is substantial and already carries device, buffer, texture,
  sampler, command encoder, render/compute pipeline, binary archive, event,
  counter, and mesh pipeline operations.
- Mesh/object render pipeline and draw calls exist and can be extended for true
  D3D12 amplification/mesh shaders.
- No complete WineMetal acceleration-structure object/descriptor/encoder API is
  exposed.
- No complete visible-function/intersection-function table API is exposed.
- No DXR shader-table binding abstraction exists.
- No D3D12-facing sparse-resource mapping API is bridged despite Metal 4
  placement sparse resources being available on the proof host.
- VRS/rasterization-rate map APIs are not exposed through the current bridge.
- Winemetal ABI validation currently catches stale PE bridge copies; the
  initial runtime preflight found an outdated prefix `system32/winemetal.dll`.
- Every new bridge call requires normal and WOW64 call-table parity, struct size
  checks, export checks, and runtime identity evidence.

### 3.9 Runtime/config audit

- `~/.metalsharp/runtime/wine/bin/wine --version` reports `wine-11.5`.
- Runtime D3D12 artifacts live under
  `~/.metalsharp/runtime/wine/lib/dxmt_m12/`.
- The general probe script defaults to `lib/dxmt`, so final M12 commands must
  pass the intended runtime explicitly until the script default is corrected.
- Runtime `dxmt.conf` currently contains:

  ```ini
  d3d11.metalSpatialUpscaleFactor = 2.00
  d3d11.preferredMaxFrameRate = 60
  d3d11.maxFeatureLevel = 12_1
  dxmt.shaderMetalVersion = 310
  ```

- The config has no D3D12 feature-level policy and still pins shader generation
  to Metal 3.1 despite the proof host and Xcode beta supporting Metal 4.
- The final runtime must not depend on the user's long-lived Steam prefix for
  probes.

## 4. Implementation phases and hard gates

### Phase 0 — Reproducible toolchain and baseline (started)

Deliverables:

- Pin `DEVELOPER_DIR` to Xcode 27 beta 6 for every Metal build/probe command.
- Pin Wine and wineserver paths to the vendored Wine 11.5 runtime.
- Keep x86_64 LLVM 15 and its sidecars on the external drive.
- Make the full DXMT artifact build work with Xcode 27 beta 6 and current MinGW.
- Add machine-readable environment evidence: Xcode build, Metal compiler build,
  SDK version, Wine version, CPU/GPU family, and runtime hashes.
- Add an isolated-prefix wrapper so probes cannot silently fall back to
  `prefix-steam`.

Hard gate:

```sh
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/prepare-dxmt-x86-llvm15.sh
```

All requested artifacts must build, and `file` must report the expected PE
x86-64 and Mach-O x86_64 architectures.

### Phase 1 — Expand the contracts and probes before advertising new caps

Deliverables:

- Upgrade `feature-support-contract.json` from the current 12_1/SM6.5 posture to
  a staged 12_2/SM6.7 target matrix.
- Add explicit fields for every official FL12_2 requirement.
- Add probes for device creation at every required feature level.
- Add focused probe groups for VRS, mesh shaders, sampler feedback, sparse/tiled
  resources, conservative raster tier 3, depth bounds, copy timestamps,
  immediate writes, unaligned BC textures, and DXR.
- Extend `validate-probe-matrix.py` so no target can be reported without a
  runnable probe token and contract entry.
- Replace environment-variable capability escape hatches with behavior-derived
  gates.
- Make unhandled/incorrectly-sized feature queries return the appropriate error
  rather than generic zeroed success.

Hard gate:

- Contract validators pass.
- New probes build before implementation and fail for the expected missing
  behavior.
- No feature report is raised in this phase.

### Phase 2 — COM, ABI, object lifetime, and diagnostic correctness

Deliverables:

- Apply `ComPrivateData` to every D3D12 object and DXGI subobject.
- Preserve and expose debug names.
- Validate `QueryInterface` coverage and vtable order through all claimed device,
  command-list, resource, fence, pipeline, DXGI, and Agility interfaces.
- Add missing interface versions needed for enhanced barriers and modern
  Agility behavior.
- Retain every object referenced by recorded commands until replay completion.
- Correct command allocator/list reset, close, reuse, and bundle lifecycle.
- Replace TODO assertions with valid HRESULT/state behavior.
- Extend WineMetal ABI contracts before adding advanced calls.

Hard gate:

- COM private-data roundtrip probe passes for every object category.
- Repeated QI/AddRef/Release/reset/replay stress completes without leaks, UAF,
  stale pointers, or vtable collisions.
- Winemetal normal/WOW64 ABI gate passes.

### Phase 3 — Core resource, heap, format, and residency completeness

Deliverables:

- Remove invalid-format BGRA fallback; fail invalid resource/view combinations.
- Implement correct 1D/2D/3D/cube/array/MSAA resource and view behavior.
- Complete plane-aware footprints, copies, resolve modes, and subresource IO.
- Implement real GPU-to-CPU readback and CPU-to-GPU upload for textures.
- Implement placed resource aliasing and aliasing barriers.
- Implement shared handles or a correct Wine/macOS process-local emulation where
  cross-process Metal sharing is not available.
- Implement sparse/reserved buffers and textures using Metal sparse/placement
  sparse APIs.
- Implement update/copy tile mappings and `GetResourceTiling`.
- Implement residency accounting and notification behavior.
- Implement sampler feedback resources/UAVs and resolve behavior.
- Prove all format support bits against executable operations.

Hard gate:

- Resource/view/format matrix has no unexplained skip.
- Sparse mapping probe writes, remaps, unmaps, and reads expected tile data.
- Reserved resources are never silently substituted by committed resources.
- Shared-handle and residency probes pass or return a documented API-accurate
  platform result without false success.

### Phase 4 — Command recording and queue replay completeness

Deliverables:

- Add command records and replay for every currently empty command-list method.
- Implement stream output, predication, depth bounds, sample positions, view
  instancing, atomic buffer copies, tiled copies, discard, and markers/events.
- Implement all immediate-write modes and queue support flags.
- Implement render pass preserve/resolve/suspend/resume semantics, not only
  clear translation.
- Implement enhanced barriers and barrier layouts through a unified state and
  hazard model.
- Add command-list 7+ interface support required by the chosen Agility header.
- Complete direct/compute/copy/bundle behavior and queue timestamp calibration.

Hard gate:

- Each command has one focused record/replay/readback test.
- Bundle and indirect execution preserve references and state.
- Barrier visibility tests pass across render, compute, copy, sparse mapping,
  acceleration structure, and present work.

### Phase 5 — Shader Models 6.0 through 6.7

Deliverables:

- Inventory every DXIL opcode and intrinsic emitted by the pinned DXC for the
  required shader models and stages.
- Turn unsupported opcode/intrinsic counters into a generated coverage ledger.
- Complete control flow, PHI, aggregates, pointers/address spaces, resource
  handles, derivatives, atomics, barriers, and wave/quad lowering.
- Complete texture dimensions, arrays, cubes, MSAA, typed/raw/structured buffers,
  counters, and bindless descriptor heap access.
- Prove group-shared allocation sizing/alignment and threadgroup metadata.
- Implement all 32-bit and 64-bit atomic operations required by reported caps.
- Implement SM6.6 dynamic resources and descriptor indexing.
- Implement all SM6.7 features listed in section 2.
- Compile generated MSL with the Xcode 27 beta 6 Metal compiler and execute it
  through Wine 11.5/DXMT.

Hard gate:

- The shader corpus includes positive and negative cases for every supported
  opcode group and stage.
- Every positive shader compiles, links, creates a PSO, executes, and validates
  output.
- Unsupported counters are zero for the required corpus.
- `D3D12_FEATURE_SHADER_MODEL` remains below 6.7 until the final corpus passes.

### Phase 6 — Geometry, tessellation, amplification, and mesh pipelines

Deliverables:

- Generalize the existing geometry-to-Metal-mesh path beyond the current probe
  subset.
- Complete hull/domain shader translation and dynamic tessellation shapes.
- Implement D3D12 amplification and mesh shader pipeline-state stream objects.
- Bind object/mesh resources and root arguments with stage-correct visibility.
- Implement direct and indirect `DispatchMesh`.
- Implement mesh pipeline statistics and render-target-array behavior.
- Prove coexistence with fragment shading rate and depth/stencil state.

Hard gate:

- GS, HS/DS, AS/MS, and mixed-stage PSO matrices pass.
- Mesh shader tier remains unreported until all required tier-1 probes pass.

### Phase 7 — Remaining feature-level 12_2 raster and memory requirements

Deliverables:

- Variable-rate shading tier 2 using Metal rasterization-rate maps or a
  semantically equivalent path.
- Conservative rasterization tier 3, including inner-input coverage and
  degenerate triangle behavior required by the tier.
- Depth bounds testing.
- Copy queue timestamp queries.
- Unaligned block-compressed textures.
- Typed format casting with castable format lists from device 10+ resource APIs.
- Tiled resources tier 3 and sampler feedback tier 0.9.
- Correct `CheckFeatureSupport` values for all FL12_2-required fields.

Hard gate:

- A dedicated `probe_feature_level_12_2` checks every required field and then
  exercises the behavior associated with each field.
- Device creation at 12_2 fails before this phase's full gate and succeeds only
  after it passes.

### Phase 8 — DXR 1.0/1.1 over Metal acceleration structures

#### 8A. WineMetal ray-tracing ABI

Add bridge objects and calls for:

- Primitive and instance acceleration-structure descriptors.
- Triangle, bounding-box, curve, motion, and instance geometry where required
  by the chosen DXR tier.
- Acceleration-structure size queries and allocations.
- Acceleration-structure command encoding: build, refit/update, copy, compact,
  and postbuild size/serialization queries.
- Visible function tables and intersection function tables.
- Pipeline linked functions and callable/intersection functions.
- Acceleration-structure resource usage and residency.

Every new call must exist in PE thunk, normal Unix table, WOW64 table, C++
facade, ObjC implementation, ABI contract, size probe, and export check.

#### 8B. D3D12 acceleration-structure objects and commands

Implement:

- BLAS/TLAS input validation and Metal descriptor construction.
- `GetRaytracingAccelerationStructurePrebuildInfo` with valid sizes and
  alignments.
- Command recording/replay for build, update/refit, clone, compact, serialize,
  deserialize, and postbuild info.
- GPU address to acceleration-structure object mapping without colliding with
  ordinary buffer VA semantics.
- Scratch/result/source lifetime and barrier tracking.
- DXR 1.1 indirect build requirements where exposed.

#### 8C. State objects and shader identifiers

Implement:

- State object and collection parsing.
- DXIL libraries, exports, renames, hit groups, shader/pipeline configs,
  global/local root signatures, associations, and existing collections.
- Stable shader identifiers.
- State object properties, stack sizes, and `AddToStateObject`.
- Local root argument layout and shader record validation.

#### 8D. DXIL ray-tracing lowering

Implement required DXIL intrinsics for:

- Ray generation, miss, closest hit, any hit, intersection, and callable code.
- TraceRay and inline ray query.
- Ray/primitive/instance/object/world builtins.
- Payload and attribute passing.
- Accept/ignore/end-search control.
- Callable shader data.
- Recursion and stack accounting.

Map the D3D12 shader binding table to Metal function tables/intersection
functions or the Metal 4 equivalent while preserving record-local root data.

#### 8E. Dispatch and synchronization

Implement:

- `SetPipelineState1`.
- `DispatchRays`, including raygen/miss/hit/callable table strides and bounds.
- Direct and indirect DXR 1.1 dispatch as required.
- UAV/AS barriers, compaction/readback visibility, and queue synchronization.

Hard gate:

- BLAS triangle and AABB build probes.
- TLAS instance probe.
- Update/refit, copy, compact, serialize, and postbuild probes.
- Inline ray query compute probe.
- Raygen/miss/hit/callable shader-table probe.
- Local/global root signature probe.
- Any-hit and custom intersection probe.
- Multi-command-list and multi-queue lifetime probe.
- Tier 1.1 is reported only after all required DXR gates pass.

### Phase 9 — DXGI, outputs, swapchains, and events

Deliverables:

- Remove TODO assertions from adapter notifications.
- Implement adapter and memory-budget change notifications.
- Implement D3D12 DXGI surface behavior needed by shared and composition paths.
- Complete output mode, ownership, gamma, duplication, and overlay policy.
- Complete swapchain 1–4 state: resize, present, tearing, frame latency,
  fullscreen, transforms, color spaces, HDR metadata, and output changes.
- Verify `dxgi.dll` and `dxgi_dxmt.dll` bootstrap/forwarding consistency.

Hard gate:

- Factory/adapter/output/swapchain probe matrix passes under Wine 11.5.
- No risky stub ledger entries remain.

### Phase 10 — Runtime staging, config, launcher, and packaging

Deliverables:

- Make the M12 runtime directory spelling and probe default consistent.
- Stage only manifest-verified artifacts.
- Ensure the PE and Unix WineMetal halves are from the same build.
- Remove stale prefix/global copies before verification, not by blind overwrite.
- Add a D3D12 feature-level setting whose accepted values include 11_0, 11_1,
  12_0, 12_1, and 12_2.
- Update `dxmt.conf` generation and runtime doctor output.
- Select Metal 4 only when the runtime and device support it; preserve a tested
  lower-Metal fallback for supported deployment targets.
- Update hashes, bundles, installer manifests, developer SDK, and release gates.

Hard gate:

- Fresh install and migration both produce a coherent M12 layout.
- Runtime doctor proves Wine 11.5, selected feature level 12_2, Metal compiler
  target, DLL hashes, Unix bridge hashes, and no game-local stale override.

### Phase 11 — Full level-by-level and regression proof

Required device levels:

- `11_0`
- `11_1`
- `12_0`
- `12_1`
- `12_2`

For each level:

- Create the device with that exact minimum level.
- Query the complete feature matrix.
- Build and run level-appropriate graphics/compute pipelines.
- Validate lower-level behavior is not accidentally routed through unsupported
  higher-level features.
- Verify requests above the implementation's maximum fail correctly.

Regression gates:

- D3D10 and D3D11 DXMT probes.
- Existing M12 mini suite.
- Agility SDK negotiation.
- DXGI factory and swapchain.
- Resource, descriptor, queue, fence, barrier, PSO, shader, and ABI probes.
- Rust/backend/Electron contract gates for changed runtime surfaces.
- Bundle/developer-SDK verification.

### Phase 12 — Bounded live-runtime validation and PR delivery

Use games only after focused probes are green. A game launch is diagnostic
coverage, not a substitute for the SDK.

For every iteration:

1. Build on the external source tree.
2. Stage to an isolated development runtime directory.
3. Create a fresh temporary prefix with Wine 11.5.
4. Copy only the verified probe-local runtime files.
5. Run one bounded probe or capture.
6. Stop the matching Wine 11.5 wineserver.
7. Copy JSON/log/shader/PSO evidence to the results directory.
8. Delete the temporary prefix.
9. Identify one failure and fix only that failure class.
10. Re-run the narrow gate, then the parent phase gate.

After all phases:

- Run the complete strict gate from a new prefix.
- Run the selected bounded game proof with feature level forced to 12_2.
- Confirm the logs show Wine 11.5 and the intended rebuilt runtime hashes.
- Confirm device creation, all required FL12_2 feature values, SM6.7, DXR, and
  required game initialization paths.
- Clean the final temporary prefix.
- Commit the evidence/roadmap updates.
- Push the branch.
- Open the PR with a prompt-to-artifact checklist and residual-risk statement.

## 5. Mandatory isolated-prefix protocol

The reusable protocol must be automated before broad runtime iteration. Its
behavior is:

```sh
WINE="$HOME/.metalsharp/runtime/wine/bin/wine"
WINESERVER="$HOME/.metalsharp/runtime/wine/bin/wineserver"
WORK="$(mktemp -d /private/tmp/metalsharp-d3d12-gate.XXXXXX)"
PREFIX="$WORK/prefix"

cleanup() {
  WINEPREFIX="$PREFIX" "$WINESERVER" -k || true
  WINEPREFIX="$PREFIX" "$WINESERVER" -w || true
  rm -rf "$WORK"
}
trap cleanup EXIT

WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" wineboot -u
# Stage verified probe-local DLLs, run one gate, copy evidence, then cleanup.
```

The wrapper must record `wine --version` and reject any value other than the
expected MetalSharp Wine 11.5 baseline unless the contract is deliberately
updated.

It must never call an unqualified `wine`, `wine64`, or `wineserver` command.

## 6. Evidence and merge-gate hierarchy

A capability claim needs all applicable layers:

1. **Source contract:** implementation path and expected behavior documented.
2. **ABI contract:** interfaces, structs, exports, and normal/WOW64 tables agree.
3. **Build gate:** PE and Unix artifacts build with the pinned toolchains.
4. **Static validator:** contract and probe matrix include the feature.
5. **Compile gate:** DXC and Metal compile the relevant shader/stage.
6. **Object gate:** D3D12 object creation succeeds with valid descriptors and
   rejects invalid descriptors correctly.
7. **Execution gate:** commands execute through Wine 11.5 and Metal.
8. **Readback/presentation gate:** output is compared with expected data.
9. **Lifecycle gate:** reset/reuse/destruction and synchronization are valid.
10. **Clean-prefix gate:** the result reproduces without old prefix state.
11. **Regression gate:** lower feature levels and D3D10/D3D11 remain green.
12. **Packaging gate:** shipped hashes match the proven build.

Passing only a validator, successful compilation, device creation, or game boot
is insufficient.

## 7. Commit and review discipline

- Keep one logical failure class per commit where practical.
- Update or add the focused probe in the same commit as behavior.
- Do not raise reported tiers/models in the implementation commit; raise them in
  a later commit after the gate is green.
- Preserve generated/runtime artifacts only when repository policy expects them.
- Record exact commands and result paths in the PR summary.
- Do not use subagents.
- Do not use automated review agents.
- Review diffs manually and run the relevant narrow gate before each commit.
- Run the complete gate before pushing the final branch.

## 8. Risk register

| Risk | Mitigation |
| --- | --- |
| False feature advertisement routes games into unimplemented code | Capability remains low until execution/readback gates pass |
| WineMetal PE/Unix ABI drift | Contracted structs, dual call-table checks, export checks, hash identity |
| Stale runtime or game-local DLL hides source result | Probe-local copies plus runtime identity probe |
| Long-lived prefix state masks initialization defects | Mandatory disposable prefix per run |
| Xcode beta compiler changes old private Metal intrinsics | Use public MSL operations and compile all embedded shaders with pinned Xcode |
| x86_64 Unix bridge links arm64 host libraries | Prefer selected SDK stubs and verify Mach-O architecture/dependencies |
| DXR object lifetime causes GPU faults | Retain resources through command completion and add lifecycle stress probes |
| Synthetic shader coverage misses game patterns | Keep synthetic gates authoritative, then add reduced repros from captures |
| Sparse/VRS/DXR API differences across Metal generations | Query host Metal feature set and preserve explicit lower-Metal behavior |
| Huge PR becomes unauditable | Phase commits, contracts, focused probes, and a final evidence matrix |

## 9. Completion audit checklist

Before declaring the goal complete, map each item below to actual evidence:

- [ ] Phase 1 audit covers D3D12, DXGI, `dxgi_dxmt`, WineMetal, `dxmt.conf`,
      shader conversion, runtime staging, and test coverage.
- [ ] This roadmap is updated as implementation discoveries change scope.
- [ ] Xcode 27 beta 6 and Metal toolchain versions are captured.
- [ ] Every runtime gate proves MetalSharp Wine 11.5.
- [ ] Every temporary prefix is stopped and deleted after evidence capture.
- [ ] D3D12 creation and feature query pass for 11_0, 11_1, 12_0, 12_1, 12_2.
- [ ] Every official FL12_2 requirement has a behavioral probe.
- [ ] SM6.7 compile/link/PSO/execute/readback corpus passes.
- [ ] DXR 1.1 acceleration structure, state object, shader table, and dispatch
      probes pass.
- [ ] D3D12/DXGI/WineMetal risky stubs and false-success paths are removed.
- [ ] Full rebuild passes.
- [ ] Full SDK strict gate passes.
- [ ] D3D10/D3D11 regressions pass.
- [ ] Runtime staging and bundle hash gates pass.
- [ ] Bounded MetalSharp Wine 11.5 launch at feature level 12_2 passes.
- [ ] Working tree contains only intended PR changes.
- [ ] Branch is pushed.
- [ ] PR is opened with evidence and no uncovered requirement.

Until every checked item has concrete evidence, this roadmap remains active and
the goal is not complete.

## 10. Progress log

### 2026-08-25 — Baseline and object-contract foundation

- Installed and verified the Xcode 27 beta 6 Metal toolchain.
- Fixed the clean external-tree build for Xcode 27 beta 6 and current MinGW.
- Added `run-isolated-probes.sh` to pin MetalSharp Wine 11.5 and always remove
  the disposable prefix.
- Added `run-source-probes.sh` to stage the current external-tree build into the
  internal MetalSharp Wine runtime temporarily and remove it after the probe.
- Added `probe_feature_levels`; it now records exact creation results for 11_0,
  11_1, 12_0, 12_1, and 12_2 plus the official FL12_2/SM6.7 target matrix.
- Corrected `D3D12CreateDevice` so invalid levels return `E_INVALIDARG` and the
  not-yet-proven 12_2 level returns `DXGI_ERROR_UNSUPPORTED` instead of false
  success. The maximum level remains centrally capped at 12_1 until the target
  gate is complete.
- Made COM private-data storage thread-safe and implemented it for 13 D3D12
  object categories.
- `probe_object_contracts` passes for device, queue, allocator, command list,
  fence, descriptor heap, heap, resource, query heap, command signature, root
  signature, pipeline library, and shader cache session.
- Both the temporary source runtime and temporary Wine prefix were confirmed
  absent after the object-contract gate.
- Removed the route-level `DXMT_D3D12_UE_SM6_COMPAT` capability override; the
  Rust and C launch paths no longer force SM6.6/atomic64 reports.
- Changed WaveOps, atomic64, and shader-model reports to the current
  behavior-backed posture.
- Replaced the false-success committed substitute for reserved resources with
  an explicit unsupported result until real Metal sparse mapping is complete.
- The conservative `probe_device_caps` gate now passes from the current source
  build while the separate FL12_2/SM6.7 target gate remains red as intended.
- Extended the WaveOps probe from compile/PSO-only coverage to six dispatched
  32-lane runtime readbacks.
- Fixed DXIL value numbering for valid type-id zero call results, completed the
  WaveOps lowering used by the corpus, and parsed Metal Shader Converter
  reflection into the D3D12 compute argument-buffer ABI.
- Wave lane/count, ballot, lane reads, any/all, reductions, min/max, and prefix
  sum now execute with zero readback mismatches under MetalSharp Wine 11.5.
- Enabled the WaveOps feature report at a fixed 32-lane range only after that
  runtime proof passed, and removed WaveOps from the unsupported ledger.
- Added the first Shader Model 6.7 execution gate: `QuadAny`/`QuadAll` now lower
  to Metal quad votes and pass a 32-thread UAV readback even though the installed
  Metal Shader Converter 3.0.6 rejects `dx.op.quadVote.i1`; the custom Xcode 27
  MSL fallback path supplies the working implementation.
