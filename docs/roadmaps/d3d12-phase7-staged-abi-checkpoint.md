# Phase 7 staged ABI checkpoint

Inspected source revision: `ce695fdf` (bounded Work Graph compute-queue probe).
This checkpoint is **not** a Phase 7 exit, clean-source build attestation, or
full-surface promotion.

## Observations

- The staged `dxmt_m12` bridge passed the Winemetal export/source-layout audit
  (contract ABI version 31). The staged and Wine builtin x64 bridge hashes matched.
- A newly created disposable Wine prefix lacked `system32/winemetal.dll`.
  The strict prefix audit failed, despite the focused Work Graph probe passing.
- `run-probes.sh --work-graph-only` disables the ABI audit internally. Omitting
  `--no-winemetal-abi` does not make this focused invocation an ABI gate.
- After copying the staged x64 `winemetal.dll` into the disposable prefix's
  `system32`, the strict audit passed with `ok=true`, `failure_count=0`.
  The existing syswow64 bridge passed its legacy-export checks.
- A subsequent Work Graph execution probe passed with `hr=0x00000000`, all
  required readback fields true, `gpu_native_provider=true`, and
  `cpu_scheduler=false`.
- `cross_queue_dispatch_exact` covers one CPU-input record submitted on a compute
  queue after host completion of preceding direct-queue work. It does **not**
  establish inter-queue GPU fence ordering or three-record compute dispatch.

Audited x64 hashes:

| Artifact | SHA-256 |
| --- | --- |
| winemetal.dll | `2707a77c9043f0ef0785c37f38b36f11c502df7f50d85d3b80125ffd0033fd8f` |
| winemetal.so | `5ca20fd6523a5b2888e5936535ec7dc7d94ab997eb2d692d947537f3bd3b20cf` |

## Reproduction

From the repository root, use a **disposable, already initialized** Wine prefix
and the matching staged runtime. Do not apply these instructions to a user's
Steam or game prefix.

```sh
runtime="$HOME/.metalsharp/runtime/wine/lib/dxmt_m12"
# Set prefix and results to dedicated scratch directories.
cp "$runtime/x86_64-windows/winemetal.dll" \
  "$prefix/drive_c/windows/system32/winemetal.dll"
python3 tools/d3d12-metal-sdk/scripts/check-winemetal-abi.py \
  --profile phase7-prefix-staged --dxmt-runtime "$runtime" \
  --wine-runtime "$HOME/.metalsharp/runtime/wine" \
  --prefix "$prefix" --results-dir "$results/abi"
WINEPREFIX="$prefix" \
DXMT_PROBE_DLL_OVERRIDES='d3d12=n,b;dxgi=n,b;d3d11=n,b;d3d10core=n,b;winemetal=n,b' \
  tools/d3d12-metal-sdk/scripts/run-probes.sh \
  --profile metalsharp --dxmt-runtime "$runtime" --work-graph-only \
  --results-dir "$results/workgraph"
```

Inspect JSON `ok`/`pass` fields, not just the runner's exit code. Local evidence
for this checkpoint is under `/private/tmp/metalsharp-phase7-abi/`: strict audit
in `run/winemetal-abi-phase7-prefix-staged.json`, and post-audit execution in
`verified/probe-workgraph-execution-metalsharp.json`. Scratch evidence is not a
committed release artifact.

## Remaining boundaries

The staged runtime manifest identifies a bundled source rather than providing a
clean-source build attestation for this checkout. Matching bridge hashes and a
source-layout audit do not prove that every staged binary was built from current
source, nor do they establish all Windows-side ABI layouts or device execution
semantics. The fresh-build follow-up below addresses this checkpoint's source-staging gap;
repeated-build reproducibility, broader regressions, all remaining phase exit
gates, and release/promotion gates remain required.

## Fresh-source sandbox follow-up

Built revision `fd91fb9f2c5eb7f4f0403351e407236d33e70837` in a new external
Meson directory, not the retained tracked build tree. Configuration used
`vendor/dxmt/build-win64.txt`, pinned x86 LLVM 15.0.7, the installed Wine toolchain,
and `-Denable_nvapi=true -Denable_nvngx=true`. Built the nine runtime targets
listed by `prepare-dxmt-x86-llvm15.sh`, then staged with
`stage-phase6-sandbox.py --profile phase7-source` (its schema name remains Phase 6).

- Staging: `ok=true`, `failure_count=0`, `source_dirty=false`.
- Strict prefix/bridge ABI audit: `ok=true`, `failure_count=0`.
- Fresh-source Work Graph execution: `pass=true`, `hr=0x00000000`.
- Fresh-source bounded video processing regression: `pass=true`.
- No runtime capability was promoted.

Fresh bridge hashes differ from the installed bundle and are recorded separately:

| Artifact | SHA-256 |
| --- | --- |
| winemetal.dll | `abf1b7d06210ecea885bcc2b7e8e48920b66507e34668b6a58acb4063875fb98` |
| winemetal.so | `f5f48834acc69596cbb89625accb62a61c7121b4de7c35ef4d4903e167445562` |

The first sandbox invocation failed before producing execution JSON: it unloaded
the selected D3D12 module and faulted. The runner still returned zero. The unique
Unix bridge alias was in the sandbox builtin directory, which was absent from
`WINEDLLPATH`. Retrying with **both** sandbox route and builtin roots passed:

```sh
# sandbox is the root passed to stage-phase6-sandbox.py.
# Expose the layout expected by run-probes.sh without touching installed Wine.
ln -s ../../runtime "$sandbox/wine/lib/dxmt"
export DXMT_PROBE_WINEDLLPATH="$sandbox/wine/lib/dxmt:$sandbox/wine/lib/wine"
# Use --dxmt-runtime "$sandbox/wine/lib/dxmt" for the focused probes.
# Before execution, copy this sandbox's winemetal.dll into the disposable
# prefix's system32 and rerun check-winemetal-abi.py with
# --wine-runtime "$sandbox/wine" (no optional-prefix exemption).
```

Local evidence remains under `/private/tmp/metalsharp-phase7-abi/`:
`source-stage/` holds staging and ABI manifests, `source-path-fixed/` the passing
Work Graph result, and `source-video/` the video result. `source-workgraph/` and
`source-debug/` retain the failed loader attempts. `build.log` records the fresh
build; `build-root.txt` identifies the external build/sandbox directory. These
are scratch artifacts, not a release bundle or an exhaustive regression gate.

### Additional affected regressions on the same sandbox

Without rebuilding or changing the source snapshot, the following focused runs
also passed (JSON checked independently of shell exit status):

| Invocation | Evidence under `/private/tmp/metalsharp-phase7-abi/` | Result |
| --- | --- | --- |
| `--cpu-texture-map-only` | `source-cpu-texture-map/probe-cpu-texture-map-metalsharp.json` | `pass=true`, `exact=true` |
| `--discard-texture-only` | `source-discard-texture/probe-discard-texture-metalsharp.json` | `pass=true`, `exact_rect_zeroing=true` |
| `--reflection-abi-only` | `source-reflection-abi/probe-reflection-abi-metalsharp.json` | `pass=true`; binding reflection and deterministic mismatch rejection |
| `--mini-only`, filter `dxr_acceleration_structures` | `source-dxr/probe-mini-dxr_acceleration_structures-metalsharp.json` | `ok=true`, `hr=0x00000000` |
| `--mini-only`, filter `mesh_object_shader_pso` | `source-mesh/probe-mini-mesh_object_shader_pso-metalsharp.json` | `ok=true`, `hr=0x00000000` |

The two mini runs used `METALSHARP_NATIVE_IRCONVERTER=1` and
`METALSHARP_MINI_PROBE_FILTER` set to the listed filter. All used the explicit
sandbox `DXMT_PROBE_WINEDLLPATH`, DLL overrides, and disposable prefix described
above. Each result directory has its own host-runtime manifest and shader cache.

The DXR probe verified its bounded mixed-geometry fallback, direct/indirect ray
dispatch, and serialization/traversal cases. This does not establish arbitrary
heterogeneous BLAS support. The mesh probe verified direct/indirect dispatch,
array-layer output, depth/blend/wireframe behavior, payload-tail readback, and
pipeline statistics for its source-owned fixture; it is not general mesh DXIL
conversion evidence. Probe fields named `tier1_matrix_complete` or
`tier1_1_matrix_complete` describe those bounded probe matrices, not exhaustive
full-surface tier certification. No capability or unsupported-ledger entry was
promoted based on these runs.
