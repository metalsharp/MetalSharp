# VKD3D D3D12 Shader Engine
**Updated:** 2026-07-08


VKD3D treats shader translation as a defined engine, not as a single converter
function. The engine boundary starts when a D3D12 game supplies DXBC or DXIL
bytecode and ends when a Metal render or compute pipeline is created, cached,
bound, and used by a draw, dispatch, or present pass.

The shader engine is also a deployment contract. Install and migration must put
known-good shader-engine material on disk, launch must seed the selected VKD3D
cache from that material, and diagnostics must write to logs instead of mixing
proof output into shader or pipeline caches.

## Engine Surfaces

- **DXIL intake:** `dxil_container`, `llvm_bitcode`, and `dxil_ir` parse DXIL
  containers, LLVM bitcode, shader model, entrypoint, values, types, and
  resource handles.
- **MSL lowering:** `msl_lowering` is the primary typed DXIL-to-MSL path. It
  owns typed value coercion, DX op lowering, vertex input handling, binding
  manifests, and targeted compatibility fallbacks. `dxil_to_msl` remains the
  fallback path and must follow the same resource-binding rules.
- **D3D12 shader compiler:** `d3d12_shader_compiler` owns runtime compilation,
  cache lookup, converter selection, generated MSL sidecars, metallib creation,
  and compile diagnostics.
- **PSO build:** `d3d12_device` and `d3d12_pipeline_state` map D3D12 graphics
  and compute PSO descriptors to Metal pipeline states.
- **Runtime binding:** command list and command queue code bind descriptor
  tables, root constants, direct buffers, vertex inputs, draw args, and present
  resources before command encoding.
- **Cache and diagnostics:** shader cache paths, MSL/metallib sidecars,
  root-signature reports, PSO manifests, and focused D3D12 trace output are part
  of the engine contract because stale or incomplete cache data can make a fixed
  shader look broken.
- **Installed corpus:** checked-in shader corpora under
  `tools/d3d12-metal-sdk/shader-corpus/` are packaged into the runtime and
  scripts/tools bundles. Install and migration validate the expected corpus
  proof file and runtime-safe material before VKD3D is considered ready.

## Runtime Material

VKD3D shader-engine material can be sourced from:

```text
~/.metalsharp/runtime/wine/share/d3d12-metal-sdk/shader-corpus/
~/.metalsharp/runtime/d3d12-metal-sdk/shader-corpus/
~/.metalsharp/scripts/tools/d3d12-metal-sdk/shader-corpus/
```

The install/migration readiness proof is the source-controlled corpus checksum
file:

```text
elden-ring-present-vb-pull-20260612/proof/SHA256SUMS
```

That proof must exist under at least one installed corpus source, and the source
must also contain runtime-safe shader-engine material. This prevents a partial
archive with one stray `.metallib` from passing VKD3D readiness.

When a game uses the VKD3D cache namespace, `shader_cache.rs` copies runtime-safe
files into:

```text
~/.metalsharp/shader-cache/vkd3d/<appid>/
```

The copied file classes are:

```text
.metallib
.air
.msl
.dxbc
.dxil
.cso
.json
.module.txt
.dxil_report.txt
```

The cache seeding step intentionally skips proof, result, log, and direct Metal
error directories. Runtime caches should contain shader-engine inputs and
sidecars, not the entire developer proof tree.

Logs are separate:

```text
~/.metalsharp/logs/vkd3d-pipeline/<appid>/
```

Pipeline caches are also separate:

```text
~/.metalsharp/pipeline-cache/vkd3d/<appid>/
```

This split matters. A shader fix is not proven by a stale `.metallib`, and a
runtime log is not a shader cache input.

## Invariants

- A DXIL `createHandle` regression must be proven against generated MSL, not
  inferred from a game frame. The present-blend regression for Elden shader
  `6f0e7d2f3cfff83c` proves the engine samples `tex0` and `tex1`, not the old
  double-counted `tex2` path.
- Every typed MSL shader must emit `metalsharp.binding_manifest.v1` so offline
  tooling can audit direct buffers, textures, samplers, and descriptor ranges.
- Root-signature coverage, PSO manifests, and shader manifests are separate
  proof layers. Passing one layer does not imply the others are correct.
- Live game captures are diagnostics only. Contract status comes from source
  contracts, converter tests, corpus replay, PSO audits, and SDK probes.
- Shader cache invalidation is part of correctness. When lowering changes,
  stale `.msl`, `.metallib`, and pipeline-cache outputs must not be used as
  proof of current behavior.
- A present or fullscreen fallback shader must preserve the draw shape it is
  replacing. For example, the procedural fallback distinguishes a 4-vertex
  triangle-strip fullscreen quad from a 3-vertex fullscreen triangle by reading
  the bound draw args instead of clamping every draw to three vertices.
- A binding-completeness failure is not only a shader failure. Root signature,
  descriptor table, direct root buffer, vertex input, render-target, and PSO
  metadata must be checked together before blaming lowering.

## Proof Layers

The engine has separate proof layers. They are cumulative, not interchangeable.

| Layer | Artifact | Proves |
| --- | --- | --- |
| DXIL intake | `.dxbc`, `.dxil`, `.module.txt`, DXIL reports | The bytecode was found, parsed, and classified. |
| Lowering | `.msl`, binding manifests | DXIL values, resources, semantics, and bindings lowered into MSL. |
| Metal compile | `.air`, `.metallib`, compiler logs | Generated MSL compiles and links under Apple's Metal toolchain. |
| Root signature | root-signature reports | Shader bindings can be matched to D3D12 root parameters and descriptor ranges. |
| PSO manifests | `pso-render-*.json`, `pso-compute-*.json` | Render/compute pipeline descriptors have enough format, topology, shader, and binding metadata. |
| Runtime binding | focused `vkd3d.log` diagnostics | Command lists bind descriptors, buffers, draw args, render targets, and resources before encoding. |
| Developer probes | `tools/d3d12-metal-sdk/results/*.json` | The pipeline behavior is reproducible without a commercial game launch. |

## Required Gates

Run the structural engine check first:

```bash
python3 tools/d3d12-metal-sdk/scripts/validate-shader-engine.py \
  --json tools/d3d12-metal-sdk/results/shader-engine-audit-metalsharp.json
```

Then run the focused native converter gate against the relevant captured corpus:

```bash
ninja -C vendor/dxmt/build-metalsharp-x64-tests tests/test_dxil_converter
vendor/dxmt/build-metalsharp-x64-tests/tests/test_dxil_converter <shader-cache>
```

For generated MSL directories, run:

```bash
python3 tools/d3d12-metal-sdk/scripts/dxil-binding-manifest-audit.py \
  --msl-dir <generated-msl> \
  --markdown tools/d3d12-metal-sdk/results/binding-manifest-audit.md
```

For captured runtime PSO/root-signature corpora, run the root-signature,
graphics PSO, and compute PSO audits before a game launch is used as evidence.

The full SDK render proof remains:

```bash
tools/ci/vkd3d-check.sh
```

For release-bundle proof, also run:

```bash
tools/bundles/verify-bundles.sh --bundle-dir app/bundles --require mac
tools/bundles/verify-developer-sdk.sh app/bundles/metalsharp-d3d12-developer-sdk.tar.zst
```

`vkd3d-check.sh` verifies the downloaded runtime/graphics archives against the
pinned digests in `tools/ci/vkd3d-bundle-hashes.tsv` (via
`tools/ci/verify-bundle-sha256.sh`) before staging them, so the gate can never
silently execute tampered bundle material.

## Contract Authority

The machine-readable source of truth is
`tools/d3d12-metal-sdk/contracts/d3d12-shader-engine-contract.json`.
It names the source surfaces, required offline gates, runtime artifacts, and
review evidence. `validate-contracts.py` includes this contract, while
`validate-shader-engine.py` verifies the engine-specific source and gate shape.

## Developer Stress Executables

Two Windows executables exist so VKD3D development does not depend on repeated
commercial game launches:

- `vkd3d_game.exe`: the PR/CI cube proof built by `tools/ci/vkd3d-check.sh`.
- `vkd3d_stress_game.exe`: the higher-pressure title-like scene built by
  `tools/d3d12-metal-sdk/scripts/vkd3d-dev.sh stress-game`.

`vkd3d_stress_game.exe` is deliberately broader than a unit probe. It starts with
a splash/movie-style pass, then renders a beach scene with water, sun, boat,
tree geometry, text, textures, shadows, unusual vertices, and repeated presents.
It should be used when changing the shader engine, PSO creation, vertex binding,
texture sampling, or present path and a game run would be too unstable.
