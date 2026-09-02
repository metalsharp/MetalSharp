# D3D12 full-surface Phase 6 graphics proof

Phase 6 is still open. This checkpoint records the first ordered-pixel-UAV
provider without promoting the full graphics claim.

## Rasterizer-ordered UAV checkpoint

`tools/d3d12-metal-sdk/probes/probe_rov` compiles a pinned DXIL pixel shader
containing `RasterizerOrderedByteAddressBuffer` and executes it through the
Metal 4 runtime with `METAL_SHADER_CONVERTER=/nonexistent`. The pixel shader
performs a load/increment/store at one pixel for three overlapping primitives.
The exact result is:

- `create_device`, root signature, PSO, and execution: `0x00000000`;
- readback: `true`;
- `uav_value=3`, `expected_uav_value=3`, `exact=true`;
- provider: `metal_raster_order_group`.

The generated MSL report contains the DXIL resource metadata and the direct
parameter qualifier:

```text
range kind=uav ... resource_kind=11 ... rasterizer_ordered=1
 device char* buf0 [[buffer(0), raster_order_group(0)]]
```

The implementation remains fail-closed for an ROV resource outside the pixel
UAV provider. `ROVsSupported` remains `FALSE` until the complete resource,
format, state, and graphics matrix is independently closed.

## Reproduction

```bash
METAL_SHADER_CONVERTER=/nonexistent \
tools/d3d12-metal-sdk/scripts/run-probes.sh \
  --profile standalone-wine \
  --wine /Users/averyfelts/.metalsharp/runtime/wine/bin/wine \
  --prefix /Users/averyfelts/.metalsharp/prefix-steam \
  --dxmt-runtime /Users/averyfelts/.metalsharp/runtime/wine/lib/dxmt_m12 \
  --results-dir /private/tmp/phase6-rov-proof \
  --rov-only
```

The generated JSON is disposable and is not committed:
`/private/tmp/phase6-rov-proof/probe-rov-standalone-wine.json`.

## Remaining Phase 6 work

The ROV checkpoint does not close Phase 6. Full graphics-stage/topology,
geometry and tessellation breadth, conservative rasterization, programmable
sample positions, barycentrics/view instancing, VRS image/layout breadth,
formats, depth bias, and complete MSAA/readback matrices remain open.
