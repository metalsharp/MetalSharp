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
one-triangle Metal BLAS, one-instance TLAS, current-size postbuild readbacks,
an inline `RayQuery` triangle hit, stable distinct raygen/miss export
identifiers, and raygen/miss shader-table records executing through
`DispatchRays` while keeping RaytracingTier conservative. Both remain breadth
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
probe, and comparison sequence:

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
  `QuadAll` execution.
- WaveOps are reported with a fixed 32-lane range after `probe-wave-ops`
  dispatches and validates lane/count, ballot, lane read, any/all, reduction,
  min/max, and prefix behavior through UAV readback.

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
- `probe-sm66-capabilities`: SM 6.6 breadth plus SM 6.7 quad-vote reporting proof.
- `probe-wave-ops`: WaveOps audit and reporting denial/proof.
- `probe-reflection-abi`: reflected shader bindings against the descriptor and
  root-signature ABI.
- `probe-graphics-pso`: graphics PSO matrix behavior and unsupported-stage
  rejection.
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

Windowed present and indexed-texture headless render checks are useful
diagnostics, but they are not hard gates in the default matrix. Enable the
headless render proof explicitly when investigating render output:

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
multi-render-target pixel outputs, cached PSO blob behavior, complex input
layouts, and explicit rejection of stream output and HS/DS tessellation:

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
and predication support status explicitly:

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
committed and placed resources, default/upload/readback heap behavior,
buffer GPU virtual addresses, 1D/2D/3D/array/mip/MSAA texture creation,
CBV/SRV/UAV/RTV/DSV creation and binding, `GetResourceAllocationInfo`,
`GetCopyableFootprints`, common color/depth/integer/normalized/sRGB format
support, and typeless view-time typing. Sparse/reserved resources are recorded
as unsupported unless they are explicitly feature-gated and backed by Metal
sparse APIs:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --resource-views-formats-only
```

For DXGI factory and swapchain/present coverage, run the phase 14.10 probes.
The factory probe validates `IDXGIFactory` through `IDXGIFactory7`, deterministic
adapter enumeration, GPU-preference enumeration, LUID lookup, and output
enumeration. The swapchain probe validates create, buffer retrieval, render to
backbuffer, present, readback, resize, fullscreen-windowed state, color-space
reporting, frame-latency waitable object behavior, and shared device/resource
ownership:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --dxgi-only

tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp \
  --swapchain-only
```

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
