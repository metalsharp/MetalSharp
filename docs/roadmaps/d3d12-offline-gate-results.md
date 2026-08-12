# D3D12 Offline Gate Results

Date: 2026-06-12

Scope: phase 7 of the offline D3D12 finish roadmap. No game, Steam, Wine,
MetalSharp backend, `winemetal.dll`, or `winemetal.so` launch/deployment step was
used.

## Gates Run

| Gate | Command | Result |
| --- | --- | --- |
| D3D12 DLL build | `ninja -C vendor/dxmt/build-metalsharp-x64 src/d3d12/d3d12.dll` | Pass |
| D3D12 vertex contract | `/tmp/test_d3d12_vertex_contract` | Pass |
| Current RE4 DXIL cache | `/tmp/test_dxil_converter /Users/alexmondello/.metalsharp/shader-cache/vkd3d/2050650` | 442 pass, 0 fail, 0 skip |
| Backup RE4 DXIL cache | `/tmp/test_dxil_converter /Users/alexmondello/.metalsharp/shader-cache/vkd3d/2050650.vertexpull-backup-20260612022154` | 299 pass, 0 fail, 0 skip |
| PSO metadata replay | `python3 vendor/dxmt/tests/dxil/replay_d3d12_pso_metadata.py --cache /Users/alexmondello/.metalsharp/shader-cache/vkd3d/2050650 --output /tmp/replay_d3d12_pso_metadata_phase7.json` | Partial, 0 failures |
| Representative MSL compile | `xcrun -sdk macosx metal -c .../570793ee26c3a8a4.msl -o /tmp/570793ee26c3a8a4.air` | Exit 0, warning only |
| Representative MSL compile | `xcrun -sdk macosx metal -c .../b2b7ba555e7d10dc.msl -o /tmp/b2b7ba555e7d10dc.air` | Exit 0, warning only |
| Process residue check | `ps -axo ... | egrep -i 'wine|steam|metalsharp|...'` | No matching leftover processes |

## Replay Harness Details

Report: `/tmp/replay_d3d12_pso_metadata_phase7.json`

- Status: `partial`
- Pipelines inspected: 582
- Failures: 0
- Warnings: 537 `input-layout-without-msl-pull`
- Access issues: 537 `missing-captured-input-layout`

Interpretation: the current RE4 PSO cache contains many render manifests captured
before the phase 3/4 `input_layout.elements` sidecar schema existed. The replay
harness therefore cannot prove exact table metadata for those older manifests
without a new offline-only capture/dump artifact. It did not find unexplained
vertex metadata failures in the artifacts it could inspect.

Phase 8 follow-up: `docs/roadmaps/d3d12-offline-cache-preflight.md` records the
same cache with the replay tool's machine-readable `replayability` summary. The
current RE4 cache is classified as `legacy-cache`: shader sidecar checks are
still useful, but strict vertex/PSO metadata replay needs a new offline
capture/dump artifact because all 537 input-layout PSOs are missing captured
`input_layout.elements`.

## Result

The offline gates required for phases 0-7 are complete for this PR branch. The
only remaining limitation is artifact age in the pre-existing RE4 PSO manifest
cache, not a failing code path in the current source-level, DXIL-converter, or
build gates.
