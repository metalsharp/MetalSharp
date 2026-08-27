# D3D12 Metal SDK

This directory is the repo-owned development SDK for D3D12 to Metal work through Wine-compatible runtimes.

MetalSharp is one host profile for this SDK. The probes are normal Windows executables that should also run under standalone Wine prefixes, DXMT development prefixes, and future host integrations.

The released `metalsharp-d3d12-developer-sdk.tar.zst` package is self-contained:
it includes this SDK source plus a staged developer Wine/DXMT runtime under
`runtime/`. See [docs/developer-runtime.md](docs/developer-runtime.md) for the
portable package layout, platform posture, and CI publish flow.

The SDK exists to make D3D12 changes evidence-driven before game-specific debugging starts. A D3D12 claim should be backed by at least one of:

- a contract entry in `contracts/`
- a probe under `probes/`
- a repeatable script under `scripts/`
- a baseline or generated result under `baselines/` or `results/`
- a documented unsupported or risky-stub entry

## Goals

- Prove the intended DXMT D3D12 runtime is loaded.
- Keep the core probes Wine-compatible and host-agnostic.
- Prove Agility SDK negotiation behaves as modern D3D12 games expect.
- Prove feature reports match implemented or explicitly emulated behavior.
- Prove resources, descriptors, shaders, queues, fences, Winemetal ABI coverage, and rendering paths through repeatable probes.
- Keep future D3D12 work accurate, repeatable, and reviewable.

## Runtime Profiles

Run the SDK against the local MetalSharp runtime through the mandatory
isolated-prefix wrapper:

```bash
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
  tools/d3d12-metal-sdk/scripts/run-isolated-probes.sh
```

The wrapper pins `~/.metalsharp/runtime/wine/bin/wine` and its matching
`wineserver`, requires the expected Wine 11.5 version, selects the isolated M12
runtime under `lib/dxmt_m12`, records tool/runtime hashes, and always stops and
deletes its temporary prefix. It rejects caller-provided `--wine`, `--prefix`,
`--profile`, and `--dxmt-runtime` overrides so a local gate cannot silently use
a persistent Steam prefix or another Wine installation. Narrow flags accepted
by `run-probes.sh` can be passed through directly, for example:

```bash
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
  tools/d3d12-metal-sdk/scripts/run-isolated-probes.sh --caps-only
```

The VRS bridge can be exercised independently against a clean source build:

```bash
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --vrs-only
```

To test the current external-tree build without overwriting the installed M12
runtime, use the source wrapper:

```bash
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/run-source-probes.sh --feature-levels-only
```

Wine 11.5 resolves DXMT's builtin PE modules from its own architecture
subdirectories before app-local copies. The source wrapper therefore makes an
APFS copy-on-write clone of vendored Wine, replaces the DXMT builtins only in
that disposable clone, stages `winemetal.so` and the matching x86_64 LLVM
sidecars, delegates to the isolated-prefix wrapper, and removes the clone and
prefix afterward. It never overwrites the installed M12 or Wine routes.

`run-probes.sh --profile metalsharp` remains the low-level runner for controlled
CI/package environments that provide their own disposable prefix lifecycle.

For fast one-behavior-at-a-time D3D12 validation without launching Steam or a
game, run the headless mini-app suite:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --mini-only
```

This builds normal Windows EXEs and runs each under Wine with the DXMT M12
runtime. Each result is written to `results/probe-mini-*-metalsharp.json`.
Select one mini executable while debugging with, for example,
`METALSHARP_MINI_PROBE_FILTER=mesh_object_shader_pso`; an empty filter runs the
whole mini suite.
The current mini suite isolates:

- `create_device`
- `command_queue`
- `swapchain_present`
- `rtv_clear`
- `compute_dispatch`
- `root_signature`
- `descriptors`
- `graphics_pso`
- `geometry_shader_pso`
- `mesh_object_shader_pso`
- `texture_sample`
- `subnautica_geometry_dxil_replay`
- `dxil_texture_color_output`
- `compute_first_use_dispatch`
- `dxr_acceleration_structures`

PRs that touch `vendor/dxmt/src/d3d12`, `vendor/dxmt/src/airconv`,
`vendor/dxmt/src/winemetal`, or `tools/d3d12-metal-sdk` are expected to keep
this mini profile green locally. Repository CI validates the contracts and
probe matrix, then prints this local command as the host-runtime gate for those
touch paths.

`mesh_object_shader_pso` now proves AS/MS pipeline streams, stage-specific
CBV/SRV resources, 32-lane mesh-stage UAV writes, texture/sampler sampling,
an eight-byte payload, two-group dispatch, and direct/indirect split-screen
readback while keeping MeshShaderTier
conservative. `dxr_acceleration_structures` proves a
one-indexed-triangle and one-procedural-AABB Metal BLAS, a cloned triangle BLAS used
then refit from an x=10 translation, queried for its compacted size, compacted,
serialized through a process-scoped compatible-driver header, copied as an
opaque buffer, deserialized, and traversed through a twelve-instance TLAS refit
from an x=10 instance translation alongside a twelve-geometry
indexed/non-indexed triangle BLAS and a procedural AABB BLAS refit from an x=10 geometry translation. The TLAS itself is serialized with its exact twelve-entry
BLAS pointer list, deserialized with a rebuilt Metal dispatch header, and used
for traversal after every source D3D12 acceleration-structure resource has
been released. Current-size
postbuild readbacks and a mask-isolated inline `RayQuery` triangle hit, stable
export identifiers with the Metal Shader Converter local-sampler GPU-address
slot preserved at bytes 16-23 and private alias hashes confined to bytes 24-31,
two export-filtered collections merged into one executable
`EXISTING_COLLECTION` pipeline with renamed imports, missing-export rejection,
and lifetime proof after releasing all external collection references, GPU-resident
local SRV/UAV/CBV and sampler descriptor-table mirrors, plus a static-sampler
record table and shader-stack-size/pipeline-stack-size contract, used by five
96-byte-stride
records, and an `AddToStateObject` hit-group alias with inherited
identifiers whose shader-table record supplies the behavior-checked local-root
constant `0x4c4f434c`, reads CBV marker `0x43425631` and SRV marker
`0x53525631`, writes UAV marker
`0x4c525557`, and executes the existing closest-hit path. Renamed miss and
callable exports receive stable, distinct identifiers and execute with local
constants from index 1 of two-record, 96-byte-stride shader tables. Shader stack-size queries return
stage/component values and invalid names return `0xffffffff`; an oversized
pipeline stack request is ignored. Raygen/triangle-any-hit/closest-hit/procedural-
intersection/callable shader-table records plus depth-2 closest-hit-to-miss
recursion executing through direct `DispatchRays` and a focused DXR 1.1
`DISPATCH_RAYS` indirect command signature with a nonzero argument-buffer
offset and an independent fourth-ray procedural readback, while keeping
RaytracingTier conservative; broader shader-table matrices remain gated.
Both remain breadth
gates rather than general feature claims.

For DXIL semantic coverage, run the reduced SM6 opcode-group probe:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --semantic-only
```

This compiles reduced DXIL shaders with DXC, warms the DXMT shader cache,
converts those dumped cache entries through MetalShaderConverter, then reruns
the shaders through D3D12 and validates UAV readbacks for float/int math,
bitcasts, buffer load/store, barriers, atomics, compute IDs, wave ops, and quad
ops. Results are written to `results/probe-dxil-semantics-*.json`; the warmup
pass is kept beside it to show the primary backend cache-miss route explicitly.
This synthetic corpus is the contract proof for the SM6/DXIL opcode groups;
`probe-shaders` remains a shader-entrypoint, root-signature, argument-binding,
and PSO smoke test.

For the full required SDK matrix, including the Winemetal ABI gate and DXIL
semantic corpus, run:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp
```

For the final strict D3D12 SDK gate, run the full rebuild, contract, layout,
probe, and comparison sequence. `prepare-dxmt-x86-llvm15.sh` deliberately cleans
retained Ninja target outputs first because Git checkouts do not preserve source
mtimes; this prevents stale PE/Unix Winemetal exports from being staged:

```bash
tools/d3d12-metal-sdk/scripts/prepare-dxmt-x86-llvm15.sh
python3 tools/d3d12-metal-sdk/scripts/stage-dxmt-runtime.py --profile metalsharp
python3 tools/d3d12-metal-sdk/scripts/validate-contracts.py
python3 tools/d3d12-metal-sdk/scripts/preflight-runtime-layout.py --profile metalsharp
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp
python3 tools/d3d12-metal-sdk/scripts/compare-contract.py --profile metalsharp
python3 tools/d3d12-metal-sdk/scripts/validate-probe-matrix.py
```

The strict gate is the merge authority for the SDK. It does not require a game
launch or a title capture. Captures from Subnautica-class titles remain useful
diagnostics, but they cannot replace probe results or contract fields.

The current honest shader feature posture is:

- `dxil_to_msl_proven: true`: the primary DXIL path produces reloadable Metal
  shader artifacts through the Metal Shader Converter / Metal IR cache path.
- `dxil_semantics_proven: true`: the reduced SM6 opcode groups execute through
  D3D12 and validate UAV readbacks.
- `synthetic_shader_corpus_proven: true`: the required synthetic shader corpus
  covers SM5 baseline, SM6 progression, resources, UAV writes, typed and
  structured buffers, texture sampling, root constants, WaveOps compile/link
  gating, and unsupported feature rejection.
- Shader Model 6.7 is the reported compliant shader model.
- The SM 6.6 runtime corpus proves root constants, descriptor indexing,
  64-bit arithmetic, atomics/barriers, and texture/sampler behavior through
  UAV readback; the SM 6.7 gate additionally proves 32-lane `QuadAny` and
  `QuadAll` execution. Its compute-stage advanced-texture subset now also
  validates variable `SampleLevel` offsets across distinct texels, exact
  `GatherRaw` packed values through a declared castable view, and
  `SampleCmpLevel` selection between independently cleared depth mip levels.
  The standalone writable-MSAA probe also compiles CS 6.7 store/load shaders
  plus a graphics shader, binds writable `RWTexture2DMS<float4>` and
  `RWTexture2DMSArray<float4,4>` resources, writes sample counts 2, 4, and 8
  in logical array slices, executes a graphics pass with a DSV, resolves both
  2D and array resources, and reads back exact sample values plus float averages
  `151.5`, `251.5`, `600.5`, and `628.5`. This proves only the focused
  R32G32B32A32_FLOAT, R16G16B16A16_FLOAT, and R8G8B8A8_UNORM subset; both
  Options14 capability fields remain conservative pending additional formats,
  render-target, and broader resolve matrices.
- The opt-in `probe-vrs` path records the 1x2/2x1/2x2/2x4/4x2/4x4
  `RSSetShadingRate` matrix and attaches Metal rasterization-rate maps. The
  2x2 case also passes a MAX/PASSTHROUGH combiner pair. A clean 64x64 pass
  produces 2112/2112/1089/1056/1056/1024 nonzero pixels; a copied
  constant `R8_UINT` 8x8 shading-rate image uses the D3D12
  PASSTHROUGH/OVERRIDE pair and independently produces 1089 for 2x2.
  Nonconstant images, broader combiner matrices, and logical-resolution
  reconstruction remain gated, so Options6 stays conservative.
- WaveOps are reported with a fixed 32-lane range after `probe-wave-ops`
  dispatches and validates lane/count, ballot, lane read, any/all, reduction,
  min/max, and prefix behavior through UAV readback.
- Options9 typed-resource/group-shared and Options11 directly-indexed descriptor-
  heap 64-bit atomics are reported through software locking. The SM 6.6 corpus
  proves 64-thread add stress and exact add/and/or/xor/signed-min/signed-max/
  unsigned-min/unsigned-max/exchange/compare-exchange matrices across raw,
  typed, group-shared, and `ResourceDescriptorHeap` paths. SIMD-cooperative
  lane selection prevents lanes in one SIMD group from spinning against each
  other while a persistent 32-bit Metal lock serializes each 64-bit critical
  section. The native Metal probe remains the hardware rationale: MSL 3.1 on
  the target M4 executes device `atomic_ulong` min/max but rejects device add
  and threadgroup add. Run that exact compiler/runtime gate with the required
  beta toolchain:

```bash
DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer \
  /usr/bin/xcrun --sdk macosx clang++ -std=c++20 -fblocks \
  -framework Foundation -framework Metal \
  tools/d3d12-metal-sdk/scripts/probe-metal-atomic64.mm \
  -o /tmp/probe-metal-atomic64
/tmp/probe-metal-atomic64
```

`dxil_semantics_proven` is supporting evidence only. It must not substitute for
`dxil_to_msl_proven`, and the contract comparator fails if shader compliance
depends only on semantic coverage.

The default required probe groups prove:

- `probe-loader`: the Wine process resolves the intended DXMT D3D12/DXGI route.
- `probe-agility-ue5`: app-local Agility SDK negotiation and modern device
  interface behavior.
- `probe-device-caps`: feature reporting, unsupported advanced features, and
  conservative capability denial.
- `probe-feature-levels`: the explicit target gate for exact device creation at
  11_0, 11_1, 12_0, 12_1, and 12_2 plus the full FL12_2/SM6.7 capability
  matrix. It is opt-in with `run-isolated-probes.sh --feature-levels-only`
  until the implementation phases make it green.
- `probe-object-contracts`: private-data byte-copy, size-query, short-buffer,
  interface lifetime, deletion, and debug-name semantics across D3D12 object
  categories. Run it with `run-source-probes.sh --object-contracts-only`.
- `probe-dxgi-factory`: factory, adapter, output, GPU-preference, and LUID
  behavior.
- `probe-resources`: committed resources, heaps, upload/readback, and basic
  resource behavior.
- `probe-queues`: command queue and fence execution.
- `probe-descriptors`: descriptor heaps, root signatures, descriptor copies,
  static samplers, and null descriptors.
- `probe-shaders`: shader entry points, root signatures, argument binding, and
  primary DXIL-to-MSL proof.
- `probe-dxil-semantics`: reduced SM6 opcode semantics with runtime readback.
- `probe-shader-corpus`: the permanent synthetic shader proof harness.
- `probe-sm66-capabilities`: SM 6.6 breadth plus SM 6.7 quad-vote,
  programmable-offset, raw-gather, and comparison-LOD compute readback proof.
- `probe-writable-msaa`: focused CS 6.7 `RWTexture2DMS` and
  `RWTexture2DMSArray` UAV emulation, sample counts 2/4/8, graphics-stage UAV
  stores with a DSV, per-sample store/load, 2D/array resolves, and exact
  readback; format and broader render-target matrices remain intentionally
  outside the reported capability.
- `probe-vrs` (opt-in): the per-draw shading-rate matrix, a copied constant
  `R8_UINT` image attachment, reduced-invocation readback, and command-list
  reset/reuse.
- `probe-wave-ops`: WaveOps audit and reporting denial/proof.
- `probe-reflection-abi`: reflected shader bindings against the descriptor and
  root-signature ABI.
- `probe-graphics-pso`: graphics PSO matrix behavior, logic-op PSO creation,
  and unsupported-stage rejection.
- `probe-render-headless`: required offscreen execution/readback, including
  `D3D12_LOGIC_OP_XOR` on render target 0.
- `probe-compute-pso`: compute PSO matrix behavior.
- `probe-command-replay`: command-list, indirect, bundle, and replay behavior.
- `probe-barriers-render-pass`: barrier, render-pass, UAV, present, and
  readback visibility.
- `probe-resource-views-formats`: resource/view/format coverage.
- `probe-mini-suite`: focused one-purpose D3D12 runtime mini-apps.

During warmup passes, `compiler_primary_cache_miss` can appear while the probe
is intentionally dumping cache inputs for conversion. Treat it as a failure only
when it appears in the final required pass or when `compare-contract.py` reports
that `dxil_to_msl_proven` is false. The final comparator is the stable summary:
it must report `pass: true`, `issues: 0`, and all required probes passing.

Windowed present remains a useful diagnostic, while the indexed-texture
headless render/readback proof is part of the default required matrix. Run the
headless proof explicitly when investigating render output:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --render-headless
```

For root-signature and descriptor binding coverage, the descriptor probe now
checks root signature 1.0/1.1 parsing, all root parameter kinds, static
samplers, register-space collisions, unbounded ranges, copied descriptors, and
null CBV/SRV/UAV descriptors:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --no-loader \
  --no-agility --no-caps --no-dxgi --no-resources --no-queues --no-shaders \
  --no-render-headless --no-mini --no-windowed-present
```

For graphics PSO coverage, run the matrix probe. It validates vertex-only,
vertex/pixel, depth-only, color-only, color+depth, MSAA, blend, write-mask,
multi-render-target pixel outputs, logic-op PSO creation, cached PSO blob
behavior, complex input layouts, and explicit rejection of stream output and
HS/DS tessellation:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --graphics-pso-only
```

For compute PSO coverage, run the compute matrix probe. It validates
descriptor-table CBV/SRV reads, UAV writes, 32-bit atomics, and
dispatch-indirect argument layout and bounds handling. It also records compute
texture-sampler and append/consume counter support status explicitly:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --compute-pso-only
```

For command recording/replay coverage, run the command replay probe. It
validates command-list close/reset/reuse ordering, multiple command lists in a
single queue execute, ExecuteIndirect dispatch behavior, and records
command-signature root constants, bundle draw replay, graphics indirect replay,
and predication support status explicitly. It also invokes
`ID3D12GraphicsCommandList7::Barrier` with global, buffer, and texture groups,
then verifies exact ordered copy readback before Options12 reports enhanced
barrier support:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --command-replay-only
```

For resource barrier and render-pass coverage, run the barrier/render-pass
probe. It validates render pass clear/store across pass splits, copy to
shader-resource transition visibility, UAV-to-UAV visibility, present
transition roundtrips, and readback visibility after render, compute, and copy
work. It also records MSAA resolve support status explicitly:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --barriers-render-pass-only
```

For resource, view, and format coverage, run the phase 14.9 probe. It validates
committed and placed resources, including native default-heap texture
placement at a Metal heap offset, recorded-resource lifetime retention,
default/upload/readback heap behavior, buffer GPU virtual addresses, 1D/2D/3D/array/mip/MSAA texture creation,
CBV/SRV/UAV/RTV/DSV creation and binding, `GetResourceAllocationInfo`,
`GetCopyableFootprints`, common color/depth/integer/normalized/sRGB format
support, and typeless view-time typing. The resource probe now creates a native
Metal placement-sparse 128x128x2 RGBA8-array reserved texture, reports two
standard 128x128 subresource tiles, maps and unmaps both array slices through
`UpdateTileMappings`, copies its mapping to a second reserved texture, and
round-trips exact 64 KiB `CopyTiles` payloads for each slice. The same gate covers one-tile R8G8_UNORM, R10G10B10A2_UNORM,
R11G11B10_FLOAT, R16G16B16A16_UNORM, and R32G32B32A32_FLOAT textures plus
one-tile and two-level R8_UNORM reserved textures. A focused two-tile
reserved-buffer path reports the 64 KiB buffer
tiling, uses MTL4 heap mappings, copies exact payloads, verifies a copied
mapping with an independent readback, and verifies zero-after-unmap (with a
full shared compatibility fallback). The single-mip 128x128x2 RGBA8 path uses
an explicit placement heap and a second reserved texture reads the same
physical tile. The same gate reads mip 1 from a standard-tiled 256x256
two-level reserved texture. Tier 3 remains conservative until broader
broader physical heap-page selection and sparse-texture mapping copies,
packed/partial-mip layouts, residency transitions, and broader formats are
covered. It also round-trips a named and unnamed process-local
`CreateSharedHandle`/`OpenSharedHandle` pair and rejects unknown handles and
missing names; cross-process sharing remains gated. The probe also creates a fully typed `R32_FLOAT` texture through
`ID3D12Device10::CreateCommittedResource3` and `CreatePlacedResource2`,
declares `R32_UINT` and `R8G8B8A8_UINT` as castable formats, validates exact
`0x3f800000` and `[0,0,128,63]` compute readbacks through those views, switches
an overlapping placed alias from `1.0` to `2.0` through aliasing barriers,
rejects a mismatched-unit-size creation list, and verifies that an undeclared
`R32_SINT` view remains null before Options12 reports relaxed format casting:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --resource-views-formats-only
```

For DXGI factory and swapchain/present coverage, run the phase 14.10 probes.
The factory probe validates `IDXGIFactory` through `IDXGIFactory7`, deterministic
adapter enumeration, GPU-preference enumeration, LUID lookup, and output
enumeration. It also validates `IDXGIAdapter3` memory-budget reporting,
duplicate-handle event registration/unregistration, initially unsignaled event
state, and safe `DXGI_ERROR_UNSUPPORTED` content-protection notification
rejection, plus `IDXGIFactory7` adapter-change event ownership, initial state,
unregistration, and unknown-cookie rejection. The swapchain probe validates create, buffer retrieval, render to
backbuffer, present, readback, resize, fullscreen-windowed state, color-space
reporting, frame-latency waitable object behavior, and shared device/resource
ownership:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --dxgi-only

tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --swapchain-only
```

The queue probe also validates the D3D12 `IDXGIDevice3` path. It places one
queue behind an unsignaled GPU wait, calls `EnqueueSetEvent`, closes the
caller's event handle, proves the event remains unsignaled, releases the wait,
and then proves the retained event is signaled only after completion markers
from every live direct, compute, and copy queue. The same probe validates
relative GPU-priority persistence and bounds, the documented default/set/reset
frame-latency state, zero-resource residency/offer/reclaim calls, invalid
residency pointer rejection, and invalid offer-priority rejection. Frame-latency
pacing itself remains a separate
swapchain gate.

Before launching Steam or a game, run the game-safe preflight:

```bash
tools/d3d12-metal-sdk/scripts/preflight-before-game.sh \
  --profile subnautica2 \
  --game-dir "/Volumes/AverySSD/SteamLibrary/steamapps/common/Subnautica2/Subnautica2/Binaries/Win64"
```

This command does not launch Steam or the game. It validates the Winemetal route
layout, runs the existing D3D12 Wine probes against the game-local staged DLLs,
and replays any dumped `.dxbc` shader corpus through MetalShaderConverter. The
probe runner supplies a probe-local Wine wrapper and `MS_ROOT` context, so a
missing or broken host `mscompatdb` rules file cannot block probes for a game
that already has its D3D12/DXMT DLLs staged. If no shader corpus exists yet, it
fails with a clear capture-needed message instead of using the game as the
debugger.

For layout-only checks while iterating on DLL staging:

```bash
python3 tools/d3d12-metal-sdk/scripts/preflight-runtime-layout.py \
  --profile subnautica2 \
  --game-dir "/Volumes/AverySSD/SteamLibrary/steamapps/common/Subnautica2/Subnautica2/Binaries/Win64"
```

For offline shader replay against a known corpus:

```bash
python3 tools/d3d12-metal-sdk/scripts/replay-shader-corpus.py \
  --profile subnautica2 \
  --corpus "/path/to/shader-cache/m12/1962700"
```

For an independent shader translation oracle, compile HLSL through
`dxc -spirv` and then `spirv-cross --msl`:

```bash
python3 tools/d3d12-metal-sdk/scripts/shadercross-oracle.py \
  --hlsl tools/d3d12-metal-sdk/probes/shadercross_oracle/stage_io_types.hlsl \
  --entry VSMain \
  --profile vs_6_6

python3 tools/d3d12-metal-sdk/scripts/shadercross-oracle.py \
  --hlsl tools/d3d12-metal-sdk/probes/shadercross_oracle/stage_io_types.hlsl \
  --entry PSMain \
  --profile ps_6_6
```

This path is not a decompiler for captured DXIL blobs. It is a reference lane
for hard D3D12-to-Metal typing questions, especially vertex `stage_in`
attribute types and integer render-target outputs. Use it when DXIL-generated
MSL compiles but Metal rejects the PSO with type mismatch errors.

For a strict Winemetal ABI/export gate before any Steam or game launch:

```bash
python3 tools/d3d12-metal-sdk/scripts/check-winemetal-abi.py \
  --profile subnautica2 \
  --game-dir "/Volumes/AverySSD/SteamLibrary/steamapps/common/Subnautica2/Subnautica2/Binaries/Win64"
```

The same check is enabled by default from `run-probes.sh`, and can be run by
itself:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --winemetal-abi-only
```

This verifies the source ABI contract in
`contracts/winemetal-bridge-contract.json`, size-checks critical PE/Unix call
structs, compares normal and WOW64 Unix-call tables, and confirms staged
runtime exports. Steam/global Wine copies must keep wrapper exports such as
`WMTSetMetalShaderCachePath`; rebuilt DXMT copies must also expose shader,
pipeline-state, binary-archive, counter-sample, shared-event, and bootstrap
bridge exports such as `MTLLibrary_newFunctionWithDescriptor` and
`MTLDevice_newLibraryWithData`.

When rebuilding the x86_64 WineMetal Unix bridge on Apple Silicon, use the
repo helper to stage an x86_64 LLVM 15 toolchain outside the internal drive and
reconfigure DXMT before linking:

```bash
METALSHARP_X86_LLVM_ROOT=/Volumes/AverySSD/toolchains \
  tools/d3d12-metal-sdk/scripts/prepare-dxmt-x86-llvm15.sh
```

This avoids the common failure where `winemetal.so` links an x86_64 target
against arm64 Homebrew LLVM libraries.

For offline Metal PSO factory checks against converted shaders:

```bash
python3 tools/d3d12-metal-sdk/scripts/offline-pso-factory.py \
  --profile subnautica2 \
  --corpus "/path/to/shader-cache/m12/1962700"
```

Without a manifest, this loads converted `.metallib` files, verifies function
lookup, and creates compute PSOs for compute shaders. With a captured manifest,
it can create render PSOs too:

```json
{
  "schema": "metalsharp.d3d12-metal.offline-pso-manifest.v1",
  "pipelines": [
    {
      "name": "captured-render-pso",
      "type": "render",
      "vertex": {"metallib": "/path/vs.metallib", "function": "Main"},
      "fragment": {"metallib": "/path/ps.metallib", "function": "Main"},
      "color_formats": ["bgra8unorm"],
      "depth_format": "depth32float",
      "sample_count": 1
    }
  ]
}
```

Run that manifest with:

```bash
python3 tools/d3d12-metal-sdk/scripts/offline-pso-factory.py \
  --profile subnautica2 \
  --manifest /path/to/pso-manifest.json
```

Failures are captured in `results/offline-pso-factory-*.json` with the exact
Metal error string from `newRenderPipelineStateWithDescriptor` or
`newComputePipelineStateWithFunction`.

DXMT now also writes captured manifests beside the shader cache as
`pso-*.json` during D3D12 PSO creation. When those files exist,
`offline-pso-factory.py` automatically prefers them over shader-only discovery:

```bash
python3 tools/d3d12-metal-sdk/scripts/offline-pso-factory.py \
  --profile subnautica2 \
  --corpus "/path/to/shader-cache/m12/1962700"
```

This is the preferred no-game proof for Subnautica-class failures: the game
captures the D3D12 descriptor once, then the SDK replays Metal PSO creation
offline until the exact descriptor succeeds or reports a stable Metal error.

If no corpus exists yet, use the bounded capture runner instead of a blind
interactive launch:

```bash
tools/d3d12-metal-sdk/scripts/capture-game-shader-corpus.sh \
  --profile subnautica2 \
  --seconds 20
```

The capture runner preflights the runtime layout, launches through the backend
for a fixed short window, kills the target, and writes
`results/shader-corpus-capture-subnautica2.json` with the newly captured `.dxbc`
files. Re-run `preflight-before-game.sh` afterward without
`--allow-empty-corpus` to prove MetalShaderConverter can replay the captured
corpus.

Run the SDK against an arbitrary Wine/DXMT runtime:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh \
  --wine /path/to/wine \
  --prefix "$HOME/wine-d3d12-test" \
  --dxmt-runtime /path/to/dxmt-runtime
```

The `--dxmt-runtime` directory should contain:

```text
x86_64-windows/
  d3d12.dll
  dxgi.dll
  dxgi_dxmt.dll
  d3d11.dll
  d3d10core.dll
  winemetal.dll
x86_64-unix/
  winemetal.so
```

The runtime preflight intentionally treats Winemetal as two routes:

- Steam/global Wine copies must preserve legacy wrapper exports such as
  `WMTSetMetalShaderCachePath`.
- Rebuilt DXMT copies must preserve those legacy exports and expose the full
  WineMetal bridge contract, including shader, PSO, binary archive, shared
  event, counter sample, and bootstrap exports.

Do not manually copy stale or ad hoc `winemetal.dll`/`winemetal.so` artifacts
into `system32`, `syswow64`, or `runtime/wine/lib/wine`. Use
`stage-dxmt-runtime.py`, which mirrors the verified rebuilt bridge into the
runtime and prefix surfaces expected by the ABI checker. The preflight is
designed to catch stale-copy regressions before Steam is launched.

Wine builtin DLLs commonly report as `C:\windows\system32\*.dll` from inside the probe even when they are backed by `WINEDLLPATH` or builtin replacement files. For D3D12, the loader probe therefore also checks ordinal `101` for `D3D12CreateDevice`, which is the important custom-runtime compatibility signal for games that import D3D12 by ordinal.

`build-probes.sh` copies the Agility SDK 1.619.3 payload into `out/bin/D3D12/` and `out/bin/D3D12/x64/` before building `probe_agility_ue5.exe`: `D3D12Core.dll`, `d3d12SDKLayers.dll`, `D3D12StateObjectCompiler.dll`, `dxil.dll`, and optional tools such as `D3D12StateObjectCompiler.exe` and `d3dconfig.exe` when present. Override `AGILITY_BIN` when testing a different extracted SDK:

```bash
AGILITY_BIN=/path/to/agility/build/native/bin/x64 \
  tools/d3d12-metal-sdk/scripts/build-probes.sh
```

The Agility probe exports `D3D12SDKVersion=619` and `D3D12SDKPath=".\\D3D12\\"`, then records app-local Agility DLL discovery, D3D12 device creation, modern `ID3D12Device*` QueryInterface behavior, `ID3D12DeviceConfiguration` root-signature serialization/deserialization, shader cache store/find, pipeline-state descriptor database store/find, and deterministic rejection for unsupported state-object cache paths as JSON.

Run just the Agility phase gate with:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --agility-only
```

The device capability probe uses the same Agility export pattern and records UE5-relevant `CheckFeatureSupport` results: feature levels, shader model, resource binding tier, wave ops, atomic64, raytracing, mesh shader, sampler feedback, stream output, reserved resources, state objects, and other advanced feature gates. Its unsupported-policy section is paired with `contracts/unsupported-api-ledger.json` so advanced features are either proven, explicitly waived, or honestly rejected.

The mesh mini-probe additionally validates amplification-payload-selected
`SV_RenderTargetArrayIndex` output. Direct and indirect `DispatchMesh` render
into both slices of a two-layer `TEXTURE2DARRAY` RTV and independently read back
exact mesh pixels from every layer and scissor half. It also requires every
remaining pixel in both slices to match the RTV-array clear color. The same
probe then binds a two-layer `D32_FLOAT` DSV: depth 0.25 passes an
exact 0.5 clear in layer 0 while depth 0.75 is rejected in layer 1. The final
pass behavior-checks layered additive color/alpha blending through
exact source, clear, and output RGBA8 values in both slices. A wireframe pass
then proves edge-only rasterization through exact per-layer foreground and
background counts. The depth matrix also proves recorded `OMSetDepthBounds`
state through shader-side emulation: 0.4–0.6 accepts stored depth 0.5,
0.6–0.9 and inverted 0.9–0.1 reject every fragment, and the same inverted
state is ignored by a PSO with depth bounds disabled. Every pass validates both
array slices with zero unexpected pixels. `PIPELINE_STATISTICS1` is reported
and verified for the focused AS/MS dispatch; Mesh tier reporting remains
disabled pending broader mixed render-state matrices and broader
shader/payload coverage.

Sampler feedback tier 0.9 is behavior-gated by two focused probes. The compute
probe creates Device8 opaque min-mip and mip-region-used resources, pairs them
with 2D and 2D-array targets, clears and writes padded software feedback maps,
and validates exact decode, encode/decode round-trip, per-mip, per-layer, and
64-lane contention results. The pixel probe executes all four SM 6.5 feedback
forms (`WriteSamplerFeedback`, Bias, Grad, and Level) through independent
graphics PSOs and requires exact `[3,2,0,2]` feedback plus `0xffbf8040` color.
Run only these gates with:

```bash
tools/d3d12-metal-sdk/scripts/run-source-probes.sh \
  --sampler-feedback-only
```

Run just the unsupported-policy phase gate with:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --caps-only
```

The DXGI factory probe records factory creation, `IDXGIFactory*` QueryInterface behavior through `IDXGIFactory7`, adapter enumeration, GPU-preference enumeration when available, LUID lookup, output enumeration, and stable adapter description fields.

## Contract Commands

Generate the first-class contract files from the current external source maps:

```bash
python3 tools/d3d12-metal-sdk/scripts/generate-contracts.py
```

Validate all required contract files:

```bash
python3 tools/d3d12-metal-sdk/scripts/validate-contracts.py
```

Validate that every required probe group in the phase matrix has a runnable
script token, contract coverage entry, and CI contract gate:

```bash
python3 tools/d3d12-metal-sdk/scripts/validate-probe-matrix.py
```

Phase 1 imports:

- `contracts/d3d12-metal-contract.json` from `/Volumes/AverySSD/metalsharp/metal-api-table/final/d3d12_to_metal_map.json`
- `contracts/agility-1.619.3-contract.json` from `/Volumes/AverySSD/metalsharp/metal-api-table/final/agility_sdk_d3d12_to_metal_map.json`
- `contracts/feature-support-contract.json`
- `contracts/dxgi-contract.json`
- `contracts/unsupported-api-ledger.json`
- `contracts/risky-stub-ledger.json`
- `contracts/winemetal-bridge-contract.json`

## Phase Discipline

Each phase should:

1. Update the SDK source or contracts.
2. Update the Obsidian roadmap.
3. Commit to the draft PR branch.
4. Update the PR summary.
5. Run a hardening pass before starting the next phase.
