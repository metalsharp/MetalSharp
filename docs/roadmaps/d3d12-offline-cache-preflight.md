# D3D12 Offline Cache Preflight

Date: 2026-06-12

Scope: phase 8 follow-up for PR #200. This keeps the next validation step
offline-only and makes the phase 7 PSO replay limitation machine-readable.

## Command

```sh
python3 vendor/dxmt/tests/dxil/replay_d3d12_pso_metadata.py \
  --cache /Users/alexmondello/.metalsharp/shader-cache/vkd3d/2050650 \
  --output /tmp/replay_d3d12_pso_metadata_phase8.json
```

## Current RE4 Cache Result

- Status: `partial`
- OK: `true`
- Render PSOs inspected: `582`
- Failures: `0`
- Warnings: `537`
- Access issues: `537`
- Warning kinds: `input-layout-without-msl-pull`
- Access issue kinds: `missing-captured-input-layout`

The new `replayability` report classifies the current cache as:

```json
{
  "vertex_metadata_status": "legacy-cache",
  "input_layout_pso_count": 537,
  "captured_input_layout_pso_count": 0,
  "missing_input_layout_pso_count": 537,
  "legacy_capture_count": 537,
  "can_strictly_replay_vertex_metadata": false,
  "needs_offline_recapture_for_strict_vertex_replay": true
}
```

## Interpretation

The current RE4 PSO cache is still useful for shader sidecar presence and broad
manifest replay checks. It is not sufficient for strict vertex/PSO metadata
replay because every render PSO with input-layout elements predates the captured
`input_layout.elements` schema.

This is an artifact coverage limitation, not a newly discovered D3D12 source
failure. The replay tool still exits successfully when there are no hard
failures, but the JSON report now states that strict vertex replay needs a new
offline capture/dump artifact before it can be considered complete.

## Strict Replay Criteria

The cache is strict-replay ready when:

- `failure_count` is `0`
- `replayability.vertex_metadata_status` is `complete`
- `replayability.can_strictly_replay_vertex_metadata` is `true`
- `replayability.missing_input_layout_pso_count` is `0`

Until those are true, use the current cache result as a shader-sidecar and legacy
manifest check, not as proof that every captured RE4 input-layout PSO was replayed
against exact D3D12 vertex metadata.
