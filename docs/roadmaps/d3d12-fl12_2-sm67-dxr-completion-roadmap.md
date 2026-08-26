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
| Shader model | At least 6.5 | 6.7 | SM 6.6 breadth plus SM 6.7 quad-vote runtime readback passed |
| Ray tracing | Tier 1.1 | Not supported | DXR 1.0/1.1 probes |
| Variable-rate shading | Tier 2 | Not supported | Per-draw and image VRS probes |
| Mesh shaders | Tier 1 | Not supported | AS/MS compile, PSO, direct and indirect dispatch |
| Sampler feedback | Tier 0.9 | Not supported | Feedback UAV creation/write/resolve probe |
| Resource binding | Tier 3 | Reported tier 3 | Unbounded/direct indexing runtime probes |
| Tiled resources | Tier 3 | Reported unsupported; reserved resources explicitly fail until sparse backing exists | Sparse mapping and residency probes |
| Conservative rasterization | Tier 3 | Tier 1 | Tier-3 edge/coverage behavior probe |
| Root signature | 1.1 | Reported 1.1 | Existing plus direct-indexing extension probes |
| Depth bounds | Supported | Unsupported/no-op | Depth-bounds render probe |
| WriteBufferImmediate | Direct, compute, bundle | Direct, compute, bundle proven and reported | Three-mode GPU-VA write/readback probe |
| GPU VA bits/resource | At least 40 on x64 | 40 | Address-range and bounds probes |
| GPU VA bits/process | At least 40 on x64 | 40 | Address-range and bounds probes |
| Wave operations | Supported | Reported true, but baseline contract rejects the claim | Wave runtime readback suite |
| Output-merger logic op | Supported | Reported true | Logic-op render/readback matrix |
| VP/RT array index from rasterizer feeder | Supported | Reported true | VS/DS/GS/MS array-index probe |
| Copy-queue timestamps | Supported | Proven and reported | Metal GPU-end timestamp resolve/readback probe |
| Fully typed/relaxed format casting | Supported | Proven and reported | Device10 castable-list creation plus declared/undeclared view runtime probe |
| Unaligned block textures | Supported | Proven and reported | 7x5 BC1 footprint/copy/readback probe |
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
- Shader model reporting now hard-caps at behavior-proven 6.7 after the SM 6.6
  breadth corpus and SM 6.7 quad-vote runtime gates pass.
- The default unhandled feature-query path zeros unknown structures and returns
  `S_OK`. That can incorrectly convert a missing implementation into a valid
  unsupported response and can hide ABI-size mistakes.
- Several values are over-reported relative to implementation, including ROVs,
  conservative rasterization tier 1, typed UAV additional formats, and some
  format atomic flags.
- Feature-level 12_2 requirements currently reported false include depth
  bounds, tiled resources, VRS, mesh shaders, sampler
  feedback and enhanced barriers.
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

- The DXBC/DXIL path now reports SM 6.7 after runtime-proving SM 6.6 root
  constants, descriptor indexing, 64-bit arithmetic, atomics/barriers, and
  texture/sampler access plus SM 6.7 quad votes.
- Wave operations are reported only after the six-case 32-lane runtime corpus
  passes exact readback.
- The source tracks unsupported intrinsic/opcode counts and rejects those
  shaders; this is useful diagnostics but not full SM6.7 coverage.
- D3D12 AS/MS pipeline-state stream subobjects now compile through Metal Shader
  Converter into Metal object/mesh functions. An eight-byte amplification
  payload, stage-specific CBV/raw-SRV bindings, 32-lane mesh UAV writes, and
  mesh-stage texture/sampler sampling execute through two-group direct
  `DispatchMesh` and indirect `DISPATCH_MESH`. The 0.5 texture sample produces
  169/181 split-screen nonzero pixels, the UAV returns `0x4d534831`, and all 32
  lane-indexed payload-derived values pass exact readback.
- Mesh tier 1 remains conservatively unreported while mixed render-state
  matrices, statistics, render-target arrays, and broader shader/payload
  coverage are still gated.
- The same Metal mesh pipeline infrastructure also executes geometry-shader
  emulation and tessellation proof shapes.
- General geometry shader support remains limited.
- Stream output is explicitly rejected.
- Native tessellation is restricted to a proof shape; unsupported shapes are
  rejected/skipped.
- Shader Model 6.7 advanced texture operations now have compute-stage runtime
  lowering and exact readback for programmable offsets, `GatherRaw`, and
  `SampleCmpLevel` across two independently cleared depth mip levels. Graphics
  stages and writable MSAA textures remain gated, so Options14 stays false.
- DXIL ray-query and raytracing intrinsics do not have a complete Metal lowering
  model.
- `GetCachedBlob` returns the caller-provided pipeline cache payload and passes
  the cache round-trip object contract.

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
- Mesh/object render pipelines now expose object/mesh buffer, texture, sampler,
  and direct/indirect draw operations used by D3D12 amplification/mesh shaders.
- The bridge exposes limited triangle/instance acceleration-structure creation
  and build operations, but not the complete update/copy/serialization surface.
- A limited visible-function-table path supports the proven raygen dispatch;
  intersection-function tables and broad shader linkage remain absent.
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
- [ ] Full SM6.7 compile/link/PSO/execute/readback corpus passes; the reporting
  breadth and quad-vote gates pass, while advanced texture-op breadth remains.
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
- Proved unaligned block-compressed texture behavior by creating a 7x5 BC1
  texture, uploading and copying two 16-byte block rows through 256-byte D3D12
  footprints, and validating exact readback; Options8 now reports support.
- Implemented copy-queue timestamp queries using Metal command-buffer GPU end
  times. Timestamp resolves register a completion handler that writes nanosecond
  results into the destination Metal buffer before the following queue fence;
  a two-query copy-list gate returns nonzero monotonic values.
- Proved all three required `WriteBufferImmediate` command-list classes and
  modes: direct, compute, and an inlined bundle execute default, marker-in, and
  marker-out writes to GPU virtual addresses and read back the exact values;
  Options3 now reports the corresponding support flags.
- Exposed `ID3D12GraphicsCommandList7` and implemented enhanced barrier command
  recording/replay. Global, buffer, and texture groups close active Metal
  encoders and establish queue order through the shared event; an exact
  copy-destination-to-copy-source buffer readback passes before Options12 reports
  `EnhancedBarriersSupported = TRUE`.
- Implemented Device10 castable-format declarations for committed and placed
  resources, including proper enhanced-layout to legacy-state normalization,
  unit/block-size list validation, and rejection of undeclared texture views.
  Fully typed committed and placed `R32_FLOAT` textures now read back the exact
  `0x3f800000` bits through declared `R32_UINT` SRVs; a second declared
  `R8G8B8A8_UINT` view returns `[0,0,128,63]`, an invalid `R16_UINT` list is
  rejected, and an undeclared `R32_SINT` SRV remains null. Options12 reports
  `RelaxedFormatCastingSupported = TRUE` only after this behavior gate passes.
- Added the compute-stage SM 6.7 advanced-texture corpus. Variable
  `SampleLevel` offsets select distinct texels and return
  `[300,341,382,383]`; `GatherRaw` returns packed `0x281e140a` four times
  through a declared `R32_UINT` alias; and `SampleCmpLevel` reads independently
  cleared depth mip values `0.25/0.75` and returns comparison results `0/1` for
  explicit LODs 0/1. The custom DXIL path now preserves binding upper bounds,
  emits typed integer gathers, programmable sample offsets/LOD, depth
  comparison sampling, and DSV mip/slice attachment selection. Options14 is
  intentionally still unreported until graphics-stage breadth and writable
  MSAA textures pass.
- Completed the Shader Model 6.7 reporting gate: the SM 6.6 corpus now
  dispatches and passes exact readback for root constants, descriptor indexing,
  64-bit arithmetic, group atomics/barriers, and texture/sampler access;
  `QuadAny`/`QuadAll` additionally pass a 32-thread SM 6.7 UAV readback. Custom
  MSL binding manifests now restore resource-use masks on fresh and cached
  pipelines so unused root ranges cannot overwrite active direct bindings.
- Corrected the source-probe loader so each staged PE `winemetal.dll` is paired
  with the matching staged `winemetal.so` through a unique temporary Wine Unix
  module registration. The previous search order silently loaded Wine's bundled
  stale Unix call table and returned uninitialized values from `SM50Initialize`.
- Split legacy SM5 buffer metadata from Metal Shader Converter buffer descriptor
  lengths in the compute argument ABI. The resource/view gate now reads back
  `[13,15,17,19]`, the synthetic SM5.0–SM6.6 shader corpus passes, and the mini
  compute, first-use compute, graphics PSO, and texture-sampling gates all pass.
- Preserved caller-provided pipeline cache blobs and implemented
  `ID3D12PipelineState::GetCachedBlob`; the graphics PSO cache round-trip gate
  now passes.
- Corrected the compute sampler gate's expected value to include the sampled
  `(10,20,30,40)` texel. CBV, two SRVs, a static sampler, and a UAV now produce
  `[106,108,110,112]`, proving compute-stage texture sampling rather than
  continuing to label the working path unsupported.
- Reconnected `probe_shaders` to the current per-executable D3D12 trace file.
  The gate now passes with observed SM5 vertex/pixel/compute conversion, DXIL
  container parsing, and a cached primary Metal Shader Converter metallib; it
  also confirms that the debug MSL backend and primary-cache-miss path were not
  used for that proof.
- Implemented Metal Shader Converter's graphics ABI: reflected vertex inputs
  now use the required attribute base 11 and vertex-buffer bind base 6, while
  reflected pixel resources use three-qword linear descriptors
  `(buffer-address, texture-or-sampler-ID, metadata)` at argument-buffer slot 2.
  The SM6 DXIL textured full-screen triangle now writes all 4096 pixels of an
  `R10G10B10A2_UNORM` render target and passes readback.
- Connected the existing Airconv geometry translation to D3D12 PSOs: linked
  SM5 vertex/geometry AIR functions now become a Metal object/mesh pipeline,
  use the D3D12 resource-binding bridge, dispatch through the geometry draw
  encoder, and render 1,352 nonzero pixels in the readback gate.
- Corrected source-runtime isolation again after proving Wine 11.5 resolves
  builtin PE modules from `<wine>/lib/wine/x86_64-windows` ahead of app-local
  copies. Source probes now use an APFS copy-on-write clone of vendored Wine,
  replace builtins only inside that disposable clone, validate the current
  source hashes, and remove the clone and prefix after every run.
- Implemented native amplification/mesh pipeline streams and command replay.
  Metal Shader Converter reflection supplies object/mesh threadgroup, payload,
  and argument-buffer metadata; stage-specific root CBVs and raw-buffer SRVs
  jointly control both AS dispatch and MS vertex scale. Direct and indirect
  `DispatchMesh` render 676 pixels in separate halves of the readback target
  through a four-byte AS-to-MS
  payload. Tier 1 remains unreported pending the remaining phase-6 breadth
  gates.
- Added an ABI-validated `MTLDevice_supportsRaytracing` Winemetal query. The
  Xcode 27 beta 6 source runtime proves the target M4 reports Metal ray-tracing
  hardware support, while D3D12 OPTIONS5 intentionally remains at no DXR tier
  until acceleration structures, state objects, shader tables, synchronization,
  and `DispatchRays` pass behavior gates.
- Added the first DXR execution gate: D3D12 triangle geometry is translated into
  a Metal primitive acceleration-structure descriptor, sized through Metal,
  allocated, encoded on the D3D12 command buffer, and retained through
  completion. The original non-indexed proof is now broadened to a 16-bit
  indexed one-triangle BLAS, which builds successfully and returns 640 bytes
  through `CURRENT_SIZE` postbuild info against a 768-byte prebuild
  allocation. D3D12 instance descriptors are transposed and translated into
  Metal user-ID descriptors; a one-instance TLAS then builds to 704 bytes and
  also passes postbuild readback. Metal Shader Converter's ray-query ABI is
  supplied with the translated TLAS header and instance-contribution address;
  an inline `RayQuery` launched from `(0,0,-2)` returns the expected triangle
  hit. Raygen DXIL libraries now create state objects backed by a linked Metal
  visible-function table and synthesized `RaygenIndirection` kernel;
  `GetShaderIdentifier`, a 32-byte shader record, `SetPipelineState1`, and
  `DispatchRays` execute a raygen UAV write of 42. Raygen and miss exports now
  receive repeatable, distinct 32-byte identifiers while unknown exports return
  null. A 64-byte-aligned miss shader-table record links a Metal visible
  function at index 2; raygen issues `TraceRay` away from the TLAS and the miss
  shader returns `0x4d495353` through its payload. A second ray intersects the
  triangle TLAS and resolves a 64-byte-aligned triangle hit-group record. A
  synthesized Metal intersection wrapper and intersection-function table invoke
  visible-function index 5 for non-opaque triangle any-hit, then visible-function
  index 3 for closest-hit. Closest-hit launches a depth-2 recursive `TraceRay`
  away from the TLAS, observes the nested miss payload, and combines it with
  the any-hit marker to return `0x52454332`. Procedural AABB geometry is now
  translated to a Metal bounding-box geometry descriptor. Its update-enabled
  BLAS starts translated by x=10 and is refit to centered geometry before
  traversal, producing a 704-byte current size against a 768-byte allocation.
  The
  update-enabled triangle BLAS is cloned through `COPY_MODE_CLONE` and reports
  the same 768-byte current size. Its source geometry starts translated by
  x=10; an in-place
  `PERFORM_UPDATE` Metal refit substitutes centered vertex geometry before the
  following TLAS traversal, proving update behavior rather than only accepting
  update flags. Metal reports a 616-byte compacted size for the updated
  768-byte allocation; `COPY_MODE_COMPACT` creates the BLAS used by a
  three-instance TLAS alongside the translated AABB and a second BLAS containing
  indexed and non-indexed triangle geometries supplied through
  `ARRAY_OF_POINTERS`, proving both the compact output and multi-geometry build
  remain traversable. The update-enabled TLAS initially places the compact BLAS
  at x=10, then an in-place `PERFORM_UPDATE` refit centers that instance before
  traversal. Instance masks isolate inline `RayQuery` traversal to the
  multi-geometry BLAS while `DispatchRays` traverses the updated compact/AABB
  paths. The refittable TLAS builds to 1,216 bytes. A collection containing the
  full linked DXIL/Metal state now feeds an executable ray-tracing pipeline via
  `EXISTING_COLLECTION`; the inherited identifiers and function tables survive
  collection lifetime and subsequent growth. `AddToStateObject` now
  creates a stable, distinct alias for the inherited triangle hit group; the
  alias identifier is installed in the first shader-table record. The record's
  local-root arguments are associated with closest-hit: constant `0x4c4f434c`,
  CBV marker `0x43425631`, and SRV marker `0x53525631` are read from the record,
  while a record-local UAV
  receives exact marker `0x4c525557`. Recursion only returns `0x52454332` when
  both local reads and the existing any-hit/nested-miss behavior are correct.
  Renamed miss and callable exports preserve their canonical Metal function
  indices while using distinct stable identifier tails; `TraceRay` and
  `CallShader` select index 1 from their two-record, 64-byte-stride tables,
  validate per-record local constants, and produce
  `0x4d495353` and `0x43414c4c`. Its second 64-byte
  hit-group record
  selects a
  procedural intersection
  wrapper, invokes visible-function index 6 to call `ReportHit`, and then invokes
  visible-function index 7 for procedural closest-hit, returning `0x50524f43`.
  The converter CLI failed to apply its maximum-attribute-size option to
  `ReportHit`; the probe therefore uses the same Metal Shader Converter 3.0.6
  API directly with a consistent eight-byte pipeline attribute configuration
  across every ray-tracing stage. A callable table at offset 320 invokes
  visible-function index 4 and returns `0x43414c4c`; the three-ray launch
  preserves the raygen sentinel `42`. RaytracingTier remains unreported pending
  mixed triangle/AABB geometry, larger geometry/instance arrays,
  serialization/deserialization, collection export filtering/merging,
  new-library state-object growth,
  local descriptor tables/samplers, and broader record-count/stride/local-data
  shader-table matrices.
