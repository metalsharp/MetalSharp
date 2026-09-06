# D3D12 Feature Level 12_2, Shader Model 6.7, and DXR Completion Roadmap

**Created:** 2026-08-25

**Status:** Scoped completion recorded; broader no-fail-closed work is tracked in the [full-surface completion roadmap](d3d12-full-surface-completion-roadmap.md).

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
| Shader model | At least 6.5 | 6.7 | SM 6.6 breadth plus SM 6.7 quad-vote and advanced texture/writable-MSAA runtime readbacks passed |
| Ray tracing | Tier 1.1 | Tier 1.1 | Mixed triangle/AABB child hits, state-object, shader-table, indirect, serialization, lifetime, and clean-prefix DXR gates passed |
| Variable-rate shading | Tier 2 | Tier 2 | Per-draw, image, `SUM`, per-primitive, logical-resolution, viewport/RT-array, lifecycle, and clean-prefix VRS gates passed |
| Mesh shaders | Tier 1 | Tier 1 | AS/MS direct/indirect, payload, layered depth/blend/wireframe, resources, statistics, and clean-prefix gates passed |
| Sampler feedback | Tier 0.9 | Tier 0.9 | Software-map UAV, all write forms, 2D/array, min-mip/mip-used, clear, encode/decode, and contention probes |
| Resource binding | Tier 3 | Reported tier 3 | Unbounded/direct indexing runtime probes |
| Tiled resources | Tier 3 | Tier 3 on the pinned MTL4/M4 path | Physical placement pages, mapping copies, packed/partial mips, 3D/array traversal and alias, unmap residency zeroing, and 64 KiB `CopyTiles` readbacks passed |
| Conservative rasterization | Tier 3 | Tier 3 for the validated reference-model rasterizer path | Edge, inner-input, degenerate, winding, clipping, and MSAA reference/readback cases passed; unsupported PSO shapes fail closed |
| Root signature | 1.1 | Reported 1.1 | Existing plus direct-indexing extension probes |
| Depth bounds | Supported | Software-emulated and reported | Depth-bounds render/readback matrix |
| WriteBufferImmediate | Direct, compute, bundle | Direct, compute, bundle proven and reported | Three-mode GPU-VA write/readback probe |
| GPU VA bits/resource | At least 40 on x64 | 40 | Address-range and bounds probes |
| GPU VA bits/process | At least 40 on x64 | 40 | Address-range and bounds probes |
| Wave operations | Supported | Reported true and accepted by the behavior-backed contract | Wave runtime readback suite |
| Output-merger logic op | Supported | Reported true | Logic-op render/readback matrix |
| VP/RT array index from rasterizer feeder | Supported | Reported true | VS/DS/GS/MS array-index probe |
| Copy-queue timestamps | Supported | Proven and reported | Metal GPU-end timestamp resolve/readback probe |
| Fully typed/relaxed format casting | Supported | Proven and reported | Device10 castable-list creation plus declared/undeclared view runtime probe |
| Unaligned block textures | Supported | Proven and reported | 7x5 BC1 footprint/copy/readback probe |
| Int64 shader ops | Supported | Reported true | Arithmetic and atomic runtime readback |
| Writable MSAA textures | CS 6.7 subset | Options14 writable-MSAA support enabled for the proven matrix | Per-sample compute/graphics stores and loads, DSV interaction, 2D/array resources, sample counts 2/4/8, resolves, and exact readback passed |

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
- Several values were over-reported relative to implementation, including ROVs,
  conservative rasterization tier 1, typed UAV additional formats, and some
  format atomic flags. ROV and conservative-rasterization reports now fail
  closed (`ROVsSupported = FALSE` and
  `D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED`); the remaining
  entries stay in the residual audit until their reports are independently
  justified.
- Feature-level 12_2 requirements currently reported false include tiled
  resources, VRS, mesh shaders, conservative rasterization, and enhanced
  barriers; depth bounds is software-emulated and reported after its exact
  render/readback matrix.
- Shared handles and opening shared heaps are `E_NOTIMPL`.
- Protected-resource, lifetime-tracker, meta-command, and state-object paths
  contain `E_NOTIMPL` returns.
- `GetRaytracingAccelerationStructurePrebuildInfo` writes zeros.
- `CreateSamplerFeedbackUnorderedAccessView` is a no-op.
- Reserved-resource creation now uses a native Metal sparse heap/texture for
  the focused 2D path and supports standard multi-mip RGBA8/R32 and R8_UNORM
  shapes plus one-tile R8G8/R10G10B10A2/R11G11B10/R16G16B16A16/
  R32G32B32A32 formats; a separate two-tile reserved-buffer path uses an MTL4
  placement-sparse buffer mapping on the proof host with a full shared
  fallback. The single-mip 128x128x2 RGBA8 path now selects an explicit
  placement heap and proves a shared physical page through a second texture.
  Unsupported dimensions still fail closed and Tier 3 remains gated on broader
  placement ownership, packed/partial mip layouts, sparse-texture
  `CopyTileMappings`, and broader residency behavior.
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
- `SetSamplePositions`
- `SetViewInstanceMask`
- protected-resource sessions
- meta-command initialization/execution
- `RSSetShadingRate`
- `RSSetShadingRateImage`

Acceleration-structure build/copy/postbuild commands, `SetPipelineState1`,
`DispatchRays`, and `DispatchMesh` now have retained command records and queue
replay. The remaining operations above still cannot be fixed only in queue
replay; recording, object retention, command-list lifecycle validation, and
replay need to be implemented together.

Additional correctness issues:

- Most command methods still do not validate closed/open state.
- `Close` rejects repeated calls, and `Reset` now requires a closed list, a
  non-null same-type allocator, and updates the retained allocator reference;
  the command-replay probe covers repeated-close and null-reset rejection.
- `ExecuteBundle` still directly splices bytes, but now propagates the bundle's
  retained object references into the parent list.
- Direct resource, descriptor-heap, root-signature, query-heap,
  command-signature, descriptor-handle, and resolvable GPU-address references
  are retained for command-list lifetime. Descriptor contents and GPU addresses
  that cannot be resolved while recording still require broader coverage.
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
- Unsupported DXGI texture formats now fail closed with `E_INVALIDARG` instead
  of silently falling back to BGRA8; the resources probe covers an `R1_UNORM`
  creation rejection. Format coverage still needs broader conformance matrices.
- Texture resources receive synthetic GPU virtual addresses, even though D3D12
  GPU VAs are buffer-oriented. Address lookup therefore mixes real and
  synthetic ranges.
- Texture `Map` is unavailable without CPU-visible backing, and GPU texture
  subresource transfers still need a real staging path.
- `ReadFromSubresource` and `WriteToSubresource` now fail closed with
  `E_NOTIMPL` when no CPU-visible backing exists; the resources probe covers
  both default-heap directions. Real GPU texture subresource IO remains
  gated.
- Default-heap placed textures now use native Metal placement heaps and
  `newTextureWithDescriptor:offset:`. A focused overlapping R32_FLOAT alias
  switches from 1.0 to 2.0 through D3D12 aliasing barriers and readback;
  broader formats, heap reuse, and aliasing matrices remain gated.
- Recorded copy, barrier, descriptor, root-signature, query, indirect, and
  GPU-address references now retain their D3D12 objects through list reset or
  destruction. Bundle execution propagates the bundle's retained references;
  a resource probe releases the upload/default caller references before replay
  and still passes exact readback.
- Reserved resources now use native Metal sparse backing for focused 2D
  RGBA8-array, R8_UNORM, R8G8_UNORM, packed 10:10:10:2, R11G11B10,
  R16G16B16A16, and R32G32B32A32 paths; a separate reserved-buffer path uses
  native MTL4 placement-sparse buffers with a full shared fallback.
  Unsupported shapes fail closed rather than using committed substitutes.
- Tile mapping/unmapping, native sparse-buffer `CopyTileMappings`, and
  per-slice/mip `CopyTiles` operations execute for those proof paths; physical
  external D3D12 heap-page selection, sparse-texture mapping copies, and
  broader residency behavior remain gated.
- Residency is implicit on Apple unified memory: `MakeResident` and `Evict`
  validate pageable arrays and reject null entries, while physical residency
  accounting/tracking remains incomplete.
- `GetCopyableFootprints` needs plane-aware and all-format validation.
- Descriptor creation and copies have existing coverage, but complete
  descriptor-heap indexing, null descriptors, counters, and acceleration
  structure SRVs require expanded contracts.
- Sampler feedback descriptors are absent.
- Shared handles now have a process-local retained-object registry with named
  lookup, duplicate-handle lifetime retention, and unknown-handle rejection;
  cross-process Metal resource transport and shared-heap reconstruction remain
  gated.

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
  Converter into Metal object/mesh functions. A sixteen-byte amplification
  payload, stage-specific CBV/raw-SRV bindings, 32-lane mesh UAV writes, and
  mesh-stage texture/sampler sampling execute through two-group direct
  `DispatchMesh` and indirect `DISPATCH_MESH`. The 0.5 texture sample produces
  169/169 split-screen nonzero pixels in render-target-array layer 0 and
  144/181 in layer 1, the UAV returns `0x4d534831`, and all 32 lane-indexed
  payload-derived values pass exact readback. The amplification payload selects
  `SV_RenderTargetArrayIndex`; D3D12 `TEXTURE2DARRAY` RTV first-slice/array-size
  metadata now maps to Metal render-pass slice and array-length state.
  The same RTV-array descriptor clears all 7,529 background pixels to exact
  red while 663 exact green mesh pixels remain across both layers, with zero
  unexpected pixels.
- A second layered AS/MS pass binds a `D32_FLOAT` two-slice DSV and a depth-
  enabled PSO. Against an exact 0.5 depth clear, the payload-selected layer 0
  triangle at depth 0.25 writes 338 exact pixels while the otherwise identical
  layer 1 triangle at depth 0.75 writes zero; all 7,854 remaining pixels retain
  the exact RTV clear color and no unexpected pixel is present.
- A third layered pass enables independent color/alpha additive blending. Both
  layers produce 338/325 exact `0xffffff80` pixels from source
  `0x80bf8040` plus clear `0x80408040`; all 7,529 background pixels retain the
  clear value and no unexpected pixel is present.
- A fourth layered pass switches the mesh PSO to wireframe rasterization and
  writes exactly 76/75 edge pixels in the two layers, versus 338/325 filled
  pixels for the same geometry. All 8,041 background pixels retain the exact
  clear value and no unexpected pixel is present.
- A fifth layered matrix proves software-emulated D3D12 depth bounds because
  the Apple M4 Metal validation layer rejects the macOS 26 native encoder API
  as unsupported. `OMSetDepthBounds` is now retained in the command stream;
  DEPTH_STENCIL1/2 pipeline streams select an instrumented pixel-shader variant
  that reads the active DSV mip/array view. Inclusive bounds 0.4–0.6 accept the
  exact 0.5 stored depth before ordinary 0.25/0.75 depth comparison, producing
  338/0 pixels. Bounds 0.6–0.9 and inverted bounds 0.9–0.1 each preserve all
  8,192 clear pixels with zero unexpected output. An otherwise identical PSO
  with `DepthBoundsTestEnable = FALSE` ignores the inverted dynamic state and
  restores the exact 338/0 ordinary-depth result. Options2 reports depth-bounds
  support only after these clean-prefix Wine 11.5 readbacks pass.
- `PIPELINE_STATISTICS1` now reports exact AS/MS invocation and primitive
  counts for the focused two-group mesh dispatch. Mesh tier 1 remains
  conservatively unreported while mixed render-state matrices beyond layered
  depth/blending/wireframe and broader shader/payload coverage are still gated.
- The same Metal mesh pipeline infrastructure also executes geometry-shader
  emulation and tessellation proof shapes.
- General geometry shader support remains limited.
- Stream output is explicitly rejected.
- Native tessellation is restricted to a proof shape; unsupported shapes are
  rejected/skipped.
- Shader Model 6.7 advanced texture operations now have compute-stage runtime
  lowering and exact readback for programmable offsets, `GatherRaw`, and
  `SampleCmpLevel` across two independently cleared depth mip levels. A
  standalone writable-MSAA probe additionally compiles CS 6.7
  `RWTexture2DMS<float4>` and `RWTexture2DMSArray<float4,4>` store/load
  shaders, binds the UAV-backed emulation, writes sample counts 2, 4, and 8
  in logical array slices, executes a graphics UAV pass with a DSV, resolves
  both 2D and array resources, and reads back exact sample values
  `[300,101,102,103,400,201,202,203,700,501,800,601,602,603,604,605,606,607]`
  plus float averages `151.5`, `251.5`, `600.5`, and `628.5`. This remains a
  focused R32G32B32A32_FLOAT, R16G16B16A16_FLOAT, and R8G8B8A8_UNORM proof;
  both Options14 capability fields remain conservative pending additional
  formats, render-target, and broader resolve matrices.
- Xcode 27 beta 6's Metal 3.1 standard library declares `atomic_ulong`, but its
  generic load/store/add/compare-exchange constraints exclude `ulong` and its
  threadgroup operations; only device `ulong` min/max is exposed under the
  dedicated `__HAVE_ATOMIC_ULONG_MIN_MAX__` path. A direct M4 Metal probe
  executed device atomic-max as exact `[17,18,19,20]`, while device atomic-add
  and threadgroup ulong atomic source were rejected at library compilation.
  This explains the earlier typed-resource zero output and group-shared PSO
  failure: full Options9/Options11 behavior needs software sidecar locks and
  cannot be reported through native Metal lowering alone. DXMT now supplies
  that software path: atomic64 shaders bypass uninstrumented converter caches,
  reserve a hidden 32-bit lock buffer, and serialize each 64-bit critical
  section with SIMD-cooperative lane selection so lanes in one SIMD group do
  not deadlock each other. Opaque-pointer LLVM globals, 64-bit `atomicrmw`, and
  `cmpxchg` are parsed and lowered for group-shared memory; directly indexed
  resource heaps are rebound by heap index after ordinary root tables. Exact
  Wine 11.5 readback passes 64-thread add stress (`2080`) and unsigned/signed
  add/and/or/xor/min/max/exchange/compare-exchange matrices for raw, typed,
  group-shared, and `ResourceDescriptorHeap` resources. Options9 typed/group
  and Options11 descriptor-heap atomic64 reports are now enabled; the native
  probe remains evidence that software emulation, not native ulong atomics,
  backs those reports.
- Sampler feedback tier 0.9 now uses opaque D3D12 texture resources backed by
  padded software maps with per-resource SIMD-cooperative locks. Device8
  feedback UAV pairing preserves the logical target dimensions and mip-region
  metadata; `ClearUnorderedAccessViewUint` initializes the opaque map and
  `ResolveSubresourceRegion` encodes/decodes standard `R8_UINT` layouts.
  Compute probes pass exact min-mip region values, 64-lane contention, all
  mip-region-used subresources, 2D-array slices, and decode/encode/decode
  round-trips. Pixel probes execute `WriteSamplerFeedback`, Bias, Grad, and
  Level as four independent graphics PSOs and read back `[3,2,0,2]` plus exact
  `0xffbf8040` color. Tier 0.9 is reported; tier 1.0 remains gated because the
  implementation intentionally guarantees only the tier-0.9 wrap/clamp and
  full-resource SRV contract.
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
- Adapter content-protection notification registration now safely returns
  `DXGI_ERROR_UNSUPPORTED` with a zero cookie. Video-memory budget event
  registration duplicates and owns the caller event through unregister, and
  budget queries signal registered events only after an observed budget change;
  the focused probe validates registration, initial unsignaled state, query,
  unregister, and rejection behavior.
- Adapter-changed event registration now duplicates and owns caller handles,
  returns a nonzero cookie, remains unsignaled while the static Metal device
  list is unchanged, releases the handle on unregister, and rejects unknown
  cookies with `DXGI_ERROR_INVALID_CALL`; the focused factory probe covers the
  complete lifecycle.
- DXGI resource subresource surfaces are stubbed.
- D3D12 DXGI surface creation is `E_NOTIMPL`.
- `IDXGIDevice3::EnqueueSetEvent` now snapshots every live D3D12 queue under a
  lifetime-safe registry, serializes a Metal shared-event completion marker
  after each queue's earlier submissions, retains a duplicate Win32 event
  handle, and signals it only after every marker completes. The queue probe
  holds one copy queue behind an unsignaled fence, proves the event does not
  fire early, closes the caller handle, releases the fence, and proves eventual
  signaling across two direct, one compute, and one copy queue.
- GPU thread priority now persists the documented relative `-7..7` range and
  rejects invalid/null calls. Maximum frame latency now defaults to 3, persists
  `1..16`, and resets on zero. Offer/reclaim validates arrays and priority,
  tracks offered resource identities, and reports preserved contents on the
  unified-memory backend; zero-resource forms are probe-covered. `Trim` and
  actual frame-latency pacing remain intentionally incomplete.
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
- The bridge exposes triangle, multi-triangle, AABB, and instance acceleration
  structure sizing/build/refit/copy/compact operations. D3D12 queue-side opaque
  serialization retains Metal structures under a process-scoped compatibility
  identifier because Metal exposes no persistent AS serialization primitive.
- Visible- and intersection-function tables execute the proven raygen, miss,
  any-hit, closest-hit, procedural-intersection, and callable linkage corpus.
- D3D12 shader tables are decoded directly during dedicated ray-dispatch replay;
  broader record and local-binding matrices remain gated.
- D3D12 sparse/reserved mapping is now bridged through native Metal sparse
  heaps, resource-state encoders, and two-tile `CopyTiles` replay for the
  proven 2D RGBA8 path, with standard two-level mip readback and native MTL4
  placement-sparse buffer mappings with a full-backed fallback. The focused
  single-mip RGBA8 placement path also reads a shared physical page through a
  second texture; broader texture page selection, mapping copies, and residency
  behavior remain gated.
- The Winemetal bridge now exposes validated rasterization-rate map creation
  and render-pass attachment. The opt-in VRS probe records `RSSetShadingRate`
  2x2, compares a clean 64x64 draw against the mapped pass (4096 versus 1089
  nonzero pixels), and reuses the command list after reset. The complete
  per-draw 1x2/2x1/2x2/2x4/4x2/4x4 matrix reads back
  2112/2112/1089/1056/1056/1024 pixels; the 2x2 draw also passes a
  MAX/PASSTHROUGH combiner pair. A copied constant `R8_UINT` 8x8 shading-rate
  image independently produces the same 1089-pixel result.
  Nonconstant image mapping, combiner semantics, logical-resolution
  reconstruction, per-primitive rates, and Tier-2 breadth remain gated.
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
  d3d12.maxFeatureLevel = 12_2
  dxmt.shaderMetalVersion = 310
  ```

- `d3d12.maxFeatureLevel` accepts `11_0`, `11_1`, `12_0`, `12_1`, and `12_2`;
  the behavior-backed build maximum is now `12_2`. The M12 route also carries
  `d3d12.maxFeatureLevel=12_2` in its reserved `DXMT_CONFIG` pair. The shader
  language pin remains Metal 3.1 for the existing converter ABI while the
  proof host uses the pinned Metal 4 toolchain.
- The final runtime must not depend on the user's long-lived Steam prefix for
  probes.

## 4A. Focused completion runway (authoritative execution order)

The original Phase 0–12 sections below remain the requirement catalogue and
historical audit. They are not a parallel backlog. From this point forward,
work only one phase in this runway at a time and do not start the next phase
until the current phase's exit gate is green. This prevents additional probes,
game captures, or optional API work from obscuring the blockers that prevent
feature level 12_2.

### Completion rules

1. Keep the current clean baseline green: the 156-target build, Winemetal
   normal/WOW64 ABI check, the 24/24 source-staged probe matrix, M12 contract
   tests, and the exact D3D10/D3D11 regression gate.
2. Every new behavior change must include a focused probe or an extension of
   an existing probe, an exact readback or rejection assertion, and a fresh
   source-staged Wine 11.5 run. A compile-only or query-only result is not an
   exit gate.
3. Every result must identify the Wine, DXMT PE, Winemetal Unix, Xcode/Metal,
   and disposable-prefix inputs. Copy evidence before stopping Wine and delete
   the prefix and temporary runtime clone afterward.
4. Never raise a feature tier, shader model, or maximum feature level in an
   implementation commit. Promotion happens only in Completion Phase 8 after
   the aggregate gate passes with the exact artifacts that will be staged.
5. Optional APIs that are not required by the claimed FL12_2 surface may stay
   explicitly unsupported and ledgered. They must not return `S_OK` while doing
   nothing, and they must not block the critical path unless the claimed
   capability reaches them.

### Current blocker matrix

| Blocker | Evidence today | Required closure |
| --- | --- | --- |
| Maximum feature level | `D3D12CreateDevice` creates all five requested levels; the build maximum and query are 12_2 | Keep the report synchronized with the passing aggregate gate |
| VRS | Full focused Tier-2 matrix passes, including per-primitive, logical 65x65 reconstruction, viewport/RT-array indexing, nonconstant indexed images, combiners, and lifecycle | No blocker in the pinned MTL4/M4 matrix |
| Mesh shaders | Tier-1 AS/MS direct/indirect, payload/resource, layered depth/blend/wireframe, array, and statistics matrix passes | No blocker in the pinned MTL4/M4 matrix; work graphs remain unsupported |
| Tiled resources | Physical placement pages, mapping copies, packed/partial mips, volume/array traversal, aliases, residency zeroing, and `CopyTiles` readbacks pass | No blocker in the pinned MTL4/M4 matrix; unsupported shapes remain explicit failures |
| Conservative rasterization | Validated software reference-model edge/inner/degenerate/clipping/MSAA cases pass | Unsupported rasterizer combinations fail closed |
| DXR | BLAS/TLAS, state objects, shader tables, inline/direct/indirect dispatch, serialization, lifetime, and flattened mixed triangle/AABB child hits pass | Cross-process opaque-data portability and unexercised broad table shapes remain explicitly unsupported |
| SM6.7/MSAA | SM6.7 breadth and Options14 advanced-op/writable-MSAA matrix pass exact readback | Unsupported formats/shapes remain rejected rather than advertised |
| Residual surfaces | Claimed paths are covered by the aggregate gate; optional unsupported APIs are ledgered and comparator-checked | Final clean staging, runtime doctor, and PR evidence remain |

### Completion Phase 0 — Freeze the baseline and the red gate (complete)

Use the existing current-source evidence as the only starting point. Re-run it
only when a later phase changes a shared path. The baseline record is:

- clean 156/156 DXMT artifact build;
- 169/169 normal and WOW64 Winemetal entries with matching ABI evidence;
- 24/24 strict source-staged probe comparison, including exact D3D10/D3D11
  clear/copy/readback;
- M12 pipeline, shader-engine, runtime-layout, contract, and 33 `m12_` tests;
- the known red result: 12_2 creation is rejected and the capability rows in
  the blocker matrix are not yet promotable.

No new feature work belongs in this phase. If this baseline regresses, fix the
regression before continuing.

### Completion Phase 1 — Make one aggregate FL12_2 gate authoritative

Build the gate before changing any capability report. It must combine the
feature-level query with the behavior results rather than trusting either one
alone.

Deliverables:

- Add or extend a repository-owned `probe_feature_level_12_2`/aggregator that
  names every required `D3D12_OPTIONS*` field and maps it to the focused probe
  that proves the field's behavior.
- Require exact runtime identity, current source hashes, a disposable prefix,
  and valid JSON for every dependency. A missing or stale result is a failure,
  not a skipped test.
- Emit a short named failure list so each iteration selects one blocker only.
- Keep `kD3D12BuildMaximumFeatureLevel` at 12_1 and all unproven tiers
  conservative while this gate is being assembled.

Exit gate: the aggregate gate runs from a clean prefix, passes its validators,
and reports the expected named blockers without changing any public capability.

### Completion Phase 2 — Close VRS Tier 2

This is the next implementation priority because the current bridge now
handles scoped nonconstant image tiles, but still lacks the full Tier-2 source
matrix and shader semantic plumbing.

Deliverables:

- Decode the complete D3D12 shading-rate image tile layout, including viewport,
  array, clear, upload/copy, unmap, and resource lifetime behavior.
- Implement both combiner stages with independent horizontal/vertical axis
  semantics, including `PASSTHROUGH`, `OVERRIDE`, `MIN`, `MAX`, and `SUM`.
- Keep the bounded software/replay path for nonconstant images: each covered
  image texel must use its own load/store render pass and intersected scissor,
  rather than being silently treated as constant. Extend it to indexed,
  geometry, tessellation, and mesh-emulated draws.
- Add per-primitive rates and their interaction with an image, viewport,
  scissor, depth/stencil, render-target arrays, and command-list reset/reuse.
- Add SV_ShadingRate output/input plumbing and prove logical-resolution
  reconstruction against a CPU reference, including viewport offsets and
  partially covered trailing tiles.

Focused gate:

- neighboring image tiles with different rates, every supported combiner and
  both axes independently;
- per-primitive and image combinations with nonconstant output patterns;
- exact logical-to-physical pixel/readback expectations, not only a total
  nonzero count;
- invalid image dimensions, uninitialized images, unsupported rates, and
  stale image resources are rejected or handled according to D3D12 semantics.

Exit gate: the clean-prefix VRS matrix passes and only then may the aggregate
gate accept `D3D12_OPTIONS6.VariableShadingRateTier >= TIER_2`.

### Completion Phase 3 — Close Mesh Shader Tier 1

Generalize the already-working AS/MS path without widening the public tier
prematurely.

Deliverables:

- Cover AS/MS pipeline streams, payload sizes, thread-group shapes, stage
  visibility, raw/typed/texture/sampler resources, and direct/indirect
  `DispatchMesh` with multiple groups.
- Exercise render-target arrays, DSV/depth comparison, blending, wireframe,
  viewport/scissor, and the VRS path from Completion Phase 2.
- Complete pipeline statistics and array-index behavior for the supported
  feeder stages; distinguish an unsupported optional statistic from a false
  success.
- Validate repeated PSO creation, command-list reset/reuse, resource release,
  and queue synchronization.

Exit gate: a clean-prefix Tier-1 matrix has exact render/UAV/readback results,
no skipped required case, and no deferred compile failure. Only then may
`D3D12_OPTIONS7.MeshShaderTier` be promoted.

### Completion Phase 4 — Close Tiled Resources Tier 3

Remove the remaining distinction between a useful sparse demonstration and a
Tier-3 resource implementation.

Deliverables:

- Give each mapped D3D12 tile an explicit physical Metal heap/page owner;
  never count full shared backing as physical sparse support.
- Implement and test `UpdateTileMappings`, `CopyTileMappings`, unmapping,
  cross-resource aliases, and mapping-copy ordering on the same queue and
  across queues.
- Cover standard and packed/partial mip layouts, 1D/2D/3D/array resources,
  supported color formats, `GetResourceTiling`, and exact 64 KiB `CopyTiles`
  upload/readback/zero-after-unmap behavior.
- Define residency transitions and validation for every mapped page. Keep
  unsupported shapes as explicit failures until their path is implemented.

Exit gate: the physical-page, mapping-copy, alias, mip/3D, residency, and
readback matrix passes from a fresh prefix. Only then may the Tier-3 report be
enabled.

### Completion Phase 5 — Implement Conservative Rasterization Tier 3

The current Tier-1 software result is not sufficient for FL12_2. Implement
the semantics instead of changing the number.

Deliverables:

- Specify the D3D12 conservative edge/coverage rules in a small reference
  model, including inner-input behavior, top-left/degenerate triangles,
  winding, clipping, viewport/scissor, and MSAA sample coverage.
- Integrate the model into the supported raster path (including mesh and
  geometry-emulated draws) with a shader or draw-replay emulation that
  preserves depth, blend, array-index, and VRS ordering.
- Add negative/unsupported validation for rasterizer descriptions that the
  emulation cannot represent; do not return a Tier-3 query for those shapes.

Exit gate: reference-model output and Metal readback agree for edge, inner,
degenerate, winding, clipping, and MSAA cases from a clean prefix. Only then
may `ConservativeRasterizationTier` be promoted to Tier 3.

### Completion Phase 6 — Close the DXR 1.1 surface

Treat the current DXR result as a strong foundation, not as a Tier-1.1 claim.

Deliverables:

- Replace the separate triangle/AABB bridge assumptions with one tagged,
  ABI-checked geometry descriptor path that can build a mixed BLAS without
  inserting nil Objective-C objects. Preserve normal/WOW64 call-table parity.
- Complete mixed-geometry prebuild, build, refit/update, TLAS instance,
  compaction, serialization/deserialization, and AS barrier/lifetime paths.
- Exercise direct and indirect ray dispatch with nonzero offsets, multiple
  record counts/strides, renamed exports, local root data, callable records,
  any-hit, custom intersection, recursion, and collection/state-object growth.
- Make serialization metadata process-independent where the API requires it;
  reject incompatible driver data rather than pretending Metal opaque handles
  are portable across processes.

Exit gate: the complete DXR 1.1 matrix passes with exact payload/UAV
readbacks, released-source/lifetime stress, and no skipped required operation.
Only then may `D3D12_OPTIONS5.RaytracingTier` be promoted.

### Completion Phase 7 — Finish SM6.7 advanced operations and writable MSAA

Close the remaining shader-model and Options14 gap without disturbing the
already-green core corpus.

Deliverables:

- Extend advanced texture operations across the supported shader stages,
  dimensions, arrays, depth/typed views, programmable offsets, raw gather,
  and `SampleCmpLevel`, with exact values for borders and mip selection.
- Broaden writable `RWTexture2DMS`/array behavior across every format and
  sample count that the implementation advertises, including compute and
  graphics UAV stores, DSV interaction, resolves, partial arrays, and exact
  readback.
- Cover quad votes/helper-lane behavior and all required DXIL diagnostics;
  unsupported opcodes must fail at pipeline creation rather than execute a
  no-op shader.

Exit gate: the positive and negative SM6.7 corpus, Options14 behavior probe,
and exact writable-MSAA matrix pass in a clean prefix. Only then may the two
Options14 fields remain enabled as behavior-backed capabilities.

### Completion Phase 8 — Remove reachable dangerous stubs, then promote 12_2

Do a bounded residual audit after the feature work, not an unbounded rewrite of
every modern D3D12 interface.

Deliverables:

- Enumerate `E_NOTIMPL`, empty bodies, and `S_OK` no-op paths reachable from
  the claimed FL12_2, SM6.7, DXR, VRS, mesh, sparse, and legacy surfaces.
- Implement the required paths (including command recording/replay and
  synchronization) or return the documented failure and add the case to the
  unsupported ledger. In particular, no claimed feature may silently drop a
  command.
- Re-run the object/lifecycle, ABI, exact legacy, and command replay gates.
- In a separate promotion commit, set the behavior-backed maximum to 12_2,
  derive the required feature fields from the completed capability gate, and
  set the staged M12 configuration to 12_2. Preserve correct rejection for
  unknown levels and requests above 12_2.

Exit gate: the aggregate FL12_2 probe creates all five requested levels,
reports the required fields, executes the associated behavior probes, and
rejects invalid/above-maximum requests. The existing 24/24 matrix and legacy
gate remain green.

### Completion Phase 9 — Final staging, live proof, and PR

Only after Completion Phase 8 is green:

- Build from a clean external tree with the pinned Xcode/LLVM toolchains.
- Stage only manifest-verified PE/Unix artifacts and verify hashes, exports,
  architectures, and matching Winemetal halves.
- Run the complete strict matrix plus every promoted opt-in probe from fresh
  prefixes; stop and delete each prefix after evidence capture.
- Run the bounded MetalSharp Wine 11.5 M12 launch with `d3d12.maxFeatureLevel
  = 12_2`, confirming the selected runtime hashes in the log. Treat game
  captures as an additional proof, never as a substitute for focused gates.
- Run runtime doctor, bundle/developer-SDK checks, Rust/backend contracts, and
  the final D3D10/D3D11 regression. Remove generated binaries, caches, and logs
  from the commit.
- Update this roadmap's checklist with artifact paths, commit the intended
  source/docs changes, push the branch, and open the PR with the evidence
  matrix and residual-risk statement.

Until this phase is complete, the goal remains active.

## 4B. Historical implementation phases and hard gates

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

- Reject invalid texture formats and resource/view combinations; the invalid-
  format BGRA fallback is removed and `R1_UNORM` creation now fails closed.
- Implement correct 1D/2D/3D/cube/array/MSAA resource and view behavior.
- Complete plane-aware footprints, copies, resolve modes, and subresource IO.
- Implement real GPU-to-CPU readback and CPU-to-GPU upload for textures.
- Implement placed resource aliasing and aliasing barriers.
- Implement shared handles or a correct Wine/macOS process-local emulation where
  cross-process Metal sharing is not available. The process-local resource
  handle path is now proven; cross-process transport remains.
- Implement sparse/reserved buffers and textures using Metal sparse/placement
  sparse APIs.
- Implement update/copy tile mappings and `GetResourceTiling`; the focused
  native 2D RGBA8/R8 plus R8G8/R10G10B10A2/R11G11B10/R16G16B16A16/
  R32G32B32A32 mapping and standard-mip `CopyTiles` paths are proven, and
  native MTL4 sparse-buffer mapping copies now have an independent readback
  gate, while external heap page selection and sparse-texture mapping copies
  remain.
- Implement residency accounting and notification behavior.
- Implement sampler feedback resources/UAVs and resolve behavior.
- Prove all format support bits against executable operations.

Hard gate:

- Resource/view/format matrix has no unexplained skip.
- Sparse mapping probe writes, remaps, unmaps, and reads expected tile data;
  the current proof covers two standard 64 KiB subresource tiles in a
  128x128 RGBA8-array reserved texture.
- Reserved resources are never silently substituted by committed resources.
- Process-local shared-handle creation/open/name/unknown-handle probes pass;
  residency pointer validation and zero-resource behavior pass; cross-process
  shared-handle transport and physical residency accounting remain gated.

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
- Implement mesh pipeline statistics and render-target-array behavior. The
  focused `PIPELINE_STATISTICS1` invocation/primitive gate now passes; broader
  statistics matrices remain.
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
- No `stub_risky` ledger entries remain; retained notification entries must be
  behavior-proven with event ownership and cleanup.

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

- [x] The Phase 1 audit covers D3D12, DXGI, `dxgi_dxmt`, WineMetal, `dxmt.conf`,
      shader conversion, runtime staging, and test coverage.
- [x] This roadmap is updated as implementation discoveries change scope.
- [x] Xcode 27 beta 6 and Metal toolchain versions are captured.
- [x] The current required and opt-in runtime gates prove MetalSharp Wine 11.5.
- [x] Temporary prefixes used by the current source/staged gates are stopped and deleted after evidence capture.
- [x] Completion Phase 0 baseline is frozen: clean build, ABI, 24/24 matrix,
      M12 tests, and exact D3D10/D3D11 regression gate.
- [x] Completion Phase 1 aggregate FL12_2 gate names every required field and
      behavior dependency and rejects stale/missing evidence.
- [x] Completion Phase 2 VRS Tier 2 passes nonconstant image, combiner,
      per-primitive, logical-resolution, viewport/RT-array, and lifecycle
      readbacks.
- [x] Completion Phase 3 Mesh Shader Tier 1 passes the broader AS/MS,
      render-state, resource, statistics, and indirect-dispatch matrix.
- [x] Completion Phase 4 Tiled Resources Tier 3 passes physical page,
      mapping-copy, packed/partial-mip, 3D/array, alias, residency, and
      `CopyTiles` readback coverage.
- [x] Completion Phase 5 Conservative Rasterization Tier 3 agrees with the
      reference model for edge, inner, degenerate, clipping, and MSAA cases.
- [x] Completion Phase 6 DXR 1.1 passes mixed geometry, state-object,
      shader-table, indirect, serialization, synchronization, and lifetime
      gates.
- [x] Completion Phase 7 closes required SM6.7 advanced operations and
      writable-MSAA breadth; Options14 is enabled only after exact readback.
- [x] Completion Phase 8 removes reachable dangerous no-op paths, promotes the
      behavior-backed reports, and creates devices at all five levels.
- [x] Full rebuild passes (`prepare-dxmt-x86-llvm15.sh`, 156/156 targets).
- [x] Current source/staged SDK strict probe and comparison gates pass (24/24, including the legacy D3D10/D3D11 gate).
- [x] D3D10/D3D11 regressions pass through the source-staged exact clear/copy/readback gate; the game harness now stages those DLLs when supplied.
- [x] Completion Phase 9 final staging proves manifest-selected PE/Unix hashes,
      runtime layout, and a fresh-prefix M12 launch at feature level 12_2.
- [x] Working tree contains only intended PR changes.
- [x] Branch is pushed.
- [x] PR #557 is opened with evidence and no uncovered required failure.

Until every checked item has concrete evidence, this roadmap remains active and
the goal is not complete.

## 10. Progress log

### 2026-08-27 — Completed final Phase 9 staging and live proof

- `tools/d3d12-metal-sdk/results/stage-runtime-fl12-2-final.json` records the
  manifest-selected source build staged into the isolated M12 runtime with 18
  artifact records, `failure_count=0`, and no PE/Unix hash mismatches.
- `tools/d3d12-metal-sdk/results/runtime-preflight-fl12-2-final.json` passes
  the final runtime layout and sidecar checks with `failure_count=0`.
- A fresh disposable Wine 11.5 clone and prefix ran the bounded `m12_game.exe`
  harness (not the title-shaped stress experiment) with
  `d3d12.maxFeatureLevel=12_2` and `dxmt.shaderMetalVersion=310`. The rebuilt
  harness requests `D3D_FEATURE_LEVEL_12_2` explicitly; the run created the
  device at `FL 49664` (`12_2`), presented 81 frames, and read back
  `bright=2009`, `chroma=2009`, checksum
  `0xd56a46f1e5743cb5`, ending with `=== m12_game.exe PASS ===`.
  The temporary runtime, prefix, and wineserver were stopped and deleted; the
  captured log is `/tmp/m12-final-roadmap-live-harness.log`.
- The aggregate gate remains `pass=true`, `promotion_ready=true`, with
  `queries=31/31`, `behaviors=31/31`, `identity=22/22`, and no blockers.

### 2026-08-27 — Promoted the behavior-backed FL12_2 matrix

- Commit `16a68a83` promotes the already-proven FL12_2 runway: the build
  maximum is `12_2`, all five requested feature levels create devices, and
  the feature-level query reports `12_2` with SM6.7, Tier-3 tiled resources,
  Tier-3 conservative rasterization, Tier-1.1 ray tracing, Tier-2 VRS, and
  Tier-1 mesh shader reports. Options14 advanced texture operations and
  writable MSAA are enabled only for the exact matrices covered by the probes.
- The full source-staged gate at
  `tools/d3d12-metal-sdk/results/fl12-2-gate-metalsharp-isolated.json` passed
  with `queries=31/31`, `behaviors=31/31`, `identity=22/22`, and no blockers.
  The gate recorded the current source commit/tree digest, pinned Wine 11.5,
  Xcode `27A5252f`, Metal `32023.921.5`, and clean-state provenance after
  excluding generated paths.
- VRS now emits both viewport and render-target array-index semantics. The
  Winemetal replay uses `setViewports`/`setScissorRects` arrays instead of
  overwriting viewport/scissor zero; the focused probe passes exact 4096-pixel
  readbacks in both array slices.
- The mixed DXR path uses the tagged triangle/AABB bridge and flattens its two
  native child BLAS records into consuming TLAS instance arrays, avoiding the
  Metal 4 mixed-descriptor crash and nested-TLAS exposure. The probe now
  reads back independent mixed triangle (`0x52454332`) and mixed AABB
  (`0x50524f43`) hits in addition to the existing direct/indirect DXR matrix.
- `compare-contract.py`, the feature-support contract, and the unsupported
  ledger now distinguish the behavior-proven advertised subsets from genuinely
  unsupported breadth; ROVs, work graphs, protected sessions, stream output,
  cross-process sharing, and other unsupported APIs remain explicit failures.

### 2026-08-27 — Corrected VRS edge-tile and volume-tile traversal

- Nonconstant VRS image replay now uses the fixed D3D12 16x16 screen-space
  image tile size instead of deriving a tile size by dividing the render target
  by the image dimensions. This preserves the logical-to-physical mapping for
  non-multiple render-target sizes and trailing image texels; VRS remains
  unpromoted because per-primitive and full logical-resolution behavior are
  still incomplete.
- The tiled-resource layout model now distinguishes volume depth from array
  slices and reports the documented 8/16/32/64/128-bit and BC standard volume
  shapes. `CopyTiles` now carries X/Y/Z coordinates, supports box regions, and
  spills non-box regions across rows, volume planes, and subsequent mip/array
  subresources while preserving the 64 KiB footprint. Sparse mapping replay
  likewise handles non-box mip/array spill and boxed array-slice mappings for
  the proven placement-texture path. These changes do not widen the
  sparse-resource report: native 3D page ownership and mapping remain a
  Completion Phase 4 blocker.

### 2026-08-27 — Made aggregate provenance and collection authoritative

- The focused probes now emit explicit promotion-subset fields for the
  incomplete rows (`tier1_matrix_complete`, `tier1_1_matrix_complete`,
  `tier3_physical_page_ownership_verified`,
  `conservative_rasterization_tier3_verified`, `sm67_breadth_complete`, and
  `options14_behavior_complete`) instead of leaving the aggregate validator
  with ambiguous missing evidence. Each remains `false` until its full
  behavior gate passes.
- `run-isolated-probes.sh` now selects the proof-host Xcode 27 beta 6 developer
  directory by default (with `METALSHARP_XCODE_ROOT` override), so captured
  environment identity records the actual Xcode and Metal toolchain instead of
  silently recording `unknown`.
- `run-probes.sh --fl12-2-gate` now enables the complete required dependency
  matrix and clears any mini-probe filter before evaluating the aggregator.
  A fresh source-staged gate run therefore reports named capability blockers
  without stale/missing dependency evidence.
- Source provenance dirtiness excludes only generated build/results/cache paths,
  and the gate now requires the recorded source checkout to be clean. A fresh
  source-staged gate run passed all recorded identity checks and reached
  `24/31` query checks, with the remaining failures limited to the
  deliberately unpromoted FL12_2 capability and behavior rows.

### 2026-08-27 — Removed unsupported ROV and conservative-raster reports

- The D3D12 options query no longer reports ROV support or Conservative
  Rasterization Tier 1: the source audit found no matching Metal ROV or
  conservative-coverage implementation behind those values. A fresh
  source-staged caps probe passes the conservative policy gate with both
  fields disabled; Tier 3 remains blocked until the edge/inner-input/
  degenerate/winding/clipping/MSAA reference path exists.

### 2026-08-27 — Narrowed the remaining work to the completion runway

- The roadmap now has one authoritative, ordered Completion Phase 0–9 runway.
  The older Phase 0–12 material is retained as the audit and requirement
  catalogue, but is no longer a parallel backlog.
- Completion Phase 0 is green on the existing clean build, ABI, 24/24
  source-staged matrix, M12 tests, and exact D3D10/D3D11 gate. The remaining
  critical blocker is intentionally explicit: feature-level 12_2 creation is
  still rejected while VRS Tier 2, Mesh Tier 1, Tiled Resources Tier 3,
  Conservative Rasterization Tier 3, DXR Tier 1.1, and the final SM6.7/MSAA
  breadth gates remain red or unpromoted.
- The next implementation phase is the aggregate FL12_2 gate, followed in
  order by VRS, mesh, tiled resources, conservative rasterization, DXR,
  SM6.7/MSAA breadth, residual-stub cleanup/report promotion, and final runtime
  staging. No capability report or maximum feature level may be raised before
  the corresponding clean-prefix behavior gate passes.

### 2026-08-27 — Added bounded nonconstant VRS image execution

- `CombineShadingRate` now implements the D3D12 axis-wise `SUM` combiner with
  saturation at 4x4; packed enum arithmetic is intentionally avoided.
- A nonconstant R8_UINT shading-rate image is no longer collapsed to its first
  texel. For ordinary draws, DXMT replays one load/store Metal render pass per
  covered image texel, intersects the pass scissor with that tile, and applies
  the exact per-tile rate. The focused 8x8 checkerboard probe passes with the
  pinned M4/Xcode 27 beta 6 readback `2320`, alongside the constant-image
  `SUM/SUM` readback `1089`.
- DXIL signature parsing and both MSL lowerers now preserve the
  `SV_ShadingRate` vertex-to-pixel semantic; the focused probe verifies the
  exact red sentinel from the transported value. Actual per-primitive rate
  selection, geometry/tessellation/mesh-emulated image draws, and
  logical-resolution reconstruction remain gated. Indexed ordinary draws now
  pass the same exact checkerboard readback; no VRS capability tier was
  promoted.

### 2026-08-27 — Correct legacy runtime staging and readback gate

- The legacy regression harness now builds and runs a repository-owned D3D11
  and D3D10 probe. Each path creates an R8G8B8A8 render target, clears it,
  copies it to a CPU-readable staging texture, maps it under a disposable Wine
  11.5 prefix, and validates all 16 pixels. Source-staged current artifacts
  pass with `d3d11_hr=0x00000000`, `d3d11_readback=true`,
  `d3d10_hr=0x00000000`, and `d3d10_readback=true`.
- The diagnosis found that the earlier game-harness reproduction was loading
  Wine's installed D3D10/D3D11 modules: `run_m12_game.sh` copied only the
  D3D12/DXGI files. The harness now stages source-built D3D10/D3D11 modules
  when present, and the isolated SDK runner makes the legacy probe a required
  gate. No D3D10/D3D11 backend change was needed.

### 2026-08-27 — Current-source required gate and VRS combiner proof

- A forced Xcode 27 beta 6 rebuild of the current source runtime followed by
  `run-source-probes.sh` completed all 23 required probe groups. The strict
  `compare-contract.py` result is `pass=true` with `issues=[]`; the temporary
  Wine 11.5 clone and disposable prefix were removed after the run.
- The current SM6.6/6.7 corpus now passes every compile, link, dispatch, and
  exact-readback case, including typed, group-shared, and descriptor-heap
  software-locked atomic64 operations. The audit reports `sm66_reportable=true`
  and `sm67_reportable=true`; the separate warmup-only report remains
  intentionally non-reportable.
- The opt-in VRS probe covers the per-draw 1x2/2x1/2x2/2x4/4x2/4x4 matrix and
  MAX/PASSTHROUGH plus constant-image PASSTHROUGH/OVERRIDE combiner paths. Its
  clean readbacks are `2112/2112/1089/1056/1056/1024`, with the copied constant
  image path at `1089`. A cross-axis constant-image matrix also verifies
  MIN(1x2,2x1)=1x1 (`4096`) and MAX(1x2,2x1)=2x2 (`1089`), correcting the
  implementation from area comparison to independent D3D12 axis comparison.
  The same result passes after staging the clean rebuild into the installed M12
  runtime. VRS remains opt-in because nonconstant image maps, SUM, broader
  combiner matrices, logical-resolution reconstruction, and Tier-2 breadth are
  still gated.
- The offscreen render/readback probe is now part of the required set and
  independently verifies `D3D12_LOGIC_OP_XOR` with exact `[255,255,255,255]`
  output. Windowed present remains optional.
- The sparse bridge now creates the focused single-mip RGBA8 reserved texture
  as a Metal 4 placement-sparse resource, maps it through the D3D12 placement
  heap, and verifies the same physical 64 KiB tile through a second reserved
  texture. Broader texture mapping-copy, packed/partial-mip, 3D, and residency
  matrices remain gated; the broader FL12_2, DXR, mesh, and packaging
  requirements remain active.
- The current clean staged M12 runtime also passes the full 24-probe matrix
  and strict comparison (`pass=true`, `issues=[]`), including the placement
  texture mapping-copy and legacy D3D10/D3D11 readback gates; its disposable
  prefix was removed after the run. The M12 pipeline contract, shader-engine
  contract, runtime-layout preflight, and `cargo test m12_` (33 tests) also
  pass.
- Unsupported ordinary texture formats now fail closed instead of being
  substituted with BGRA8. A clean Wine 11.5 resources probe rejects
  `DXGI_FORMAT_R1_UNORM` with `E_INVALIDARG` while all existing resource,
  sparse, alias, mapping-copy, and BC1 readbacks remain exact.

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
  native Metal sparse-backed 2D resources, `GetResourceTiling`, queue mapping /
  unmapping, and an exact two-tile `CopyTiles` proof. Added a focused reserved
  two-tile buffer compatibility path with 64 KiB tiling, exact copies, and
  zero-after-unmap verification over full shared backing. Standard mip 1
  readback now also passes for a 256x256 two-level reserved texture. Full Tier
  3 reporting remains gated on physical D3D12 heap-page selection, aliases,
  packed/partial mips, `CopyTileMappings`, residency transitions, and broader
  layouts.
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
  comparison sampling, and DSV mip/slice attachment selection. The separate
  writable-MSAA probe now proves a focused CS 6.7 2D/array per-sample path.
  Both Options14 fields remain intentionally conservative until graphics-stage
  breadth passes.
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
  twelve-instance TLAS alongside the translated AABB and a second BLAS containing
  twelve alternating indexed and non-indexed triangle geometries supplied through
  `ARRAY_OF_POINTERS`, proving both the compact output and multi-geometry build
  remain traversable beyond the former eight-descriptor command limit. The
  multi-geometry BLAS reports 1,600 bytes against a 1,792-byte prebuild result.
  The update-enabled TLAS initially places the compact BLAS
  at x=10, then an in-place `PERFORM_UPDATE` refit centers that instance before
  traversal. Instance masks isolate inline `RayQuery` traversal to the
  multi-geometry BLAS while `DispatchRays` traverses the updated compact/AABB
  paths. The twelve-instance refittable TLAS builds to 3,136 bytes against a
  3,328-byte prebuild result. BLAS and TLAS builds now emit inline
  `SERIALIZATION` postbuild records. `COPY_MODE_SERIALIZE` writes the standard
  D3D12 serialized header, a process-scoped driver identifier, and for TLAS the
  exact twelve-entry bottom-level pointer list. Compatible, changed-version,
  unrelated, and unsupported-type identifier checks return the required four
  statuses. The BLAS blob survives `CopyBufferRegion` before
  `COPY_MODE_DESERIALIZE`; both the deserialized BLAS and a separately
  deserialized TLAS execute the existing inline and shader-table traversal
  corpus after all source D3D12 acceleration-structure resources are released.
  Persistent cross-process reconstruction remains gated and stale
  process identifiers are rejected rather than over-reported. A collection containing the
  full linked DXIL/Metal state now feeds two export-filtered derived collections;
  one imports raygen, miss, triangle-hit, and procedural-hit records while the
  other imports callable plus renamed miss/callable records. Those collections
  merge into one executable ray-tracing pipeline via `EXISTING_COLLECTION`, a
  missing requested export is rejected, a deliberately filtered closest-hit
  identifier remains unavailable, and every 32-byte identifier preserves the
  Metal Shader Converter `localRootSignatureSamplersBuffer` field at bytes
  16-23 while keeping private alias hashes in the reserved tail at bytes 24-31.
  The inherited identifiers and shared
  function tables survive release of the source and both derived collections.
  Independently linked collection merging remains fail-closed because it needs
  a Metal function-table relink. `AddToStateObject` now
  creates a stable, distinct alias for the inherited triangle hit group; the
  alias identifier is installed in the first shader-table record. The record's
  local-root arguments are associated with closest-hit: constant `0x4c4f434c`,
  mirrored CBV-table marker `0x43425631` and mirrored SRV-table marker
  `0x53525631` are read from the record, while mirrored SRV/UAV and sampler
  tables supply the resource and `SampleLevel` paths; a static local sampler
  table is supplied through `IRShaderIdentifier.localRootSignatureSamplersBuffer`.
  `GetShaderStackSize` now returns deterministic valid stage/component values,
  invalid names return `0xffffffff`, and a configured pipeline stack size
  survives an invalid oversized update. The
  record-local UAV receives exact marker `0x4c525557`. Recursion only returns
  `0x52454332` when both local reads, sampling, and the existing
  any-hit/nested-miss behavior are correct.
  Renamed miss and callable exports preserve their canonical Metal function
  indices while using distinct stable identifier tails; `TraceRay` and
  `CallShader` select index 1 from their two-record, 96-byte-stride tables,
  validate per-record local constants and descriptor-table pointers, and
  produce `0x4d495353` and `0x43414c4c`. Its second 96-byte
  hit-group record
  selects a
  procedural intersection
  wrapper, invokes visible-function index 6 to call `ReportHit`, and then invokes
  visible-function index 7 for procedural closest-hit, returning `0x50524f43`.
  The converter CLI failed to apply its maximum-attribute-size option to
  `ReportHit`; the probe therefore uses the same Metal Shader Converter 3.0.6
  API directly with a consistent eight-byte pipeline attribute configuration
  across every ray-tracing stage. A callable table at offset 448 invokes
  visible-function index 4 and returns `0x43414c4c`; the three-ray launch
  preserves the raygen sentinel `42`. A focused DXR 1.1 indirect-dispatch
  record now uses a native `D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS`
  command signature and an upload argument buffer at a nonzero 16-byte
  argument offset; queue replay decodes the 104-byte
  `D3D12_DISPATCH_RAYS_DESC` and runs it through the same shader-table path. The direct three-ray launch is copied before the output is cleared;
  the four-ray indirect launch then produces an independent procedural marker
  at the otherwise untouched fourth-ray slot, so recording alone cannot make
  this gate pass. A bounded DXR 1.2 boundary probe now accepts CONFIG1 with
  ALLOW_OPACITY_MICROMAPS, decodes one level-0 OC1 two-state OMM array, deep-
  copies OMM records, reports exact 256-byte provider sizes, exercises 128-byte
  input/array and 4-byte descriptor alignment, builds/refits a single OMM BLAS,
  and proves opaque/transparent inline RayQuery visibility. ALLOW_DISABLE_OMMS
  retains a separate ordinary BLAS selected by DISABLE_OMMS; FORCE_OMM_2_STATE
  is a two-state no-op. Four-state/unknown/mixed/opaque/update-disable forms
  remain fail-closed. OPTIONS22 truthfully reports non-reordering SER. The
  bounded probe now also passes from a committed clean-source snapshot: its
  `source_dirty=false` host identity, clean stage, shader-converter outputs,
  and passing result are retained under
  `/Volumes/AverySSD/phase8-omm-ser-slice/clean-source-runtime12/`. The phase
  evidence remains partial because native SER ordering, broader OMM layouts,
  compaction/serialization, and broader clean-source phase promotion are not
  proven. The reproducible Runtime12 result and staging manifest remain under
  `/Volumes/AverySSD/phase8-omm-ser-slice/{results,stage}`.
  RaytracingTier remains truthfully reported as 1.1; broader DXR promotion
  remains pending on mixed triangle/AABB geometry in one BLAS, persistent
  cross-process
  serialization reconstruction, independently linked collection merging,
  new-library state-object growth and broader record-count, stride, and
  local-data shader-table matrices. The resource gate also now covers a
  separate two-tile reserved-buffer path: native MTL4 heap mapping, 64 KiB
  tiling, exact tile copies, copied-mapping readback, and zero-after-unmap
  readback pass on the proof host. R8_UNORM one-tile, two-level standard-mip,
  and one packed-tail/partial-mip copies also pass. One-tile
  R8G8/R10G10B10A2/R11G11B10/R16G16B16A16/R32G32B32A32 copies pass as well; its
  fallback is explicitly not treated as physical sparse heap-page support;
  broader sparse-texture mapping-copy, packed/partial-mip, 3D, and residency
  matrices remain gated.
- The clean source-built MetalSharp Wine 11.5 profile after the indirect-DXR,
  tiled-resource, native-sparse-buffer, and writable-MSAA changes passes all
  `23/23` required contract probes. The writable-MSAA extension is independently
  covered by
  `probe-writable-msaa`, which passes CS 6.7 DXIL compilation, pipeline
  creation, writable `RWTexture2DMS`/`RWTexture2DMSArray` UAV emulation, all
  four per-sample store/load operations in both 2D and array resources, a
  graphics UAV store for both 2D and array resources with a DSV, sample-count-
  2/4/8 float resolves, and exact readback
  `[300,101,102,103,400,201,202,203,700,501,800,601,602,603,604,605,606,607,192,65,66,67]`
  plus resolve averages `151.5`, `251.5`, `600.5`, `628.5`, and the
  normalized R8 resolve value `98`. This remains a focused behavior proof;
  both Options14 fields remain conservative until additional format,
  render-target, and broader resolve matrices pass. The matching Winemetal source audit reports `169/169`
  normal/WOW64 call-table entries and `failure_count=0`, including the MTL4
  sparse-texture update/copy calls; intentional warmup and missing-capture diagnostics remain
  outside the required set, and the FL12_2 target gate remains conservative.
