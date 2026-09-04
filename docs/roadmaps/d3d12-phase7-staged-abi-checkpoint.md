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
semantics. Fresh reproducible builds, provenance validation, broader regressions,
all remaining phase exit gates, and release/promotion gates remain required.
