# D3D12 full-surface Phase 6 graphics proof

Phase 6 is still open. This checkpoint records the first ordered-pixel-UAV
provider without promoting the full graphics claim.

## Rasterizer-ordered UAV checkpoint

`tools/d3d12-metal-sdk/probes/probe_rov` compiles pinned DXIL pixel shaders
containing `RasterizerOrderedByteAddressBuffer`,
`RasterizerOrderedTexture2D<uint>`, and
`RasterizerOrderedStructuredBuffer<ROVValue>`, then executes them through the
Metal 4 runtime with `METAL_SHADER_CONVERTER=/nonexistent`. Each pixel shader
performs a load/increment/store at one pixel for three overlapping primitives.
The exact result is:

- all three create-device/root-signature/PSO/execute HRESULTs:
  `0x00000000`;
- all three readbacks: `true`;
- `byte_address_buffer=3`, `typed_texture2d_uint=3`, and
  `structured_buffer=3`, each with `expected_uav_value=3` and `exact=true`;
- provider: `metal_raster_order_group`.

The generated MSL report contains the DXIL resource metadata and the direct
parameter qualifier:

```text
range kind=uav ... rasterizer_ordered=1
 device char* buf0 [[buffer(0), raster_order_group(0)]]
 texture2d<uint, access::read_write> tex0 [[texture(0), raster_order_group(0)]]
```

The implementation remains fail-closed for an ROV resource outside the pixel
UAV provider. `ROVsSupported` remains `FALSE` until the complete resource,
format, state, and graphics matrix is independently closed.

## Binary64 emulation report

The typed DXIL semantic corpus passes all 18 binary64 cases, including basic
arithmetic, bitcast/make/split, dynamic arithmetic, IEEE-754 add/multiply/
divide/remainder/compare matrices, float and integer conversions, unary and
special-value predicates, and FMA. The runtime reports
`DoublePrecisionFloatShaderOps` from the emulation provider. This is exact
bit-level emulation and does not claim native Metal double-precision ALU.

The remaining minimum-precision report stays conservative pending a dedicated
conversion/rounding matrix. The semantic reproduction is:

```bash
METAL_SHADER_CONVERTER=/nonexistent \
tools/d3d12-metal-sdk/scripts/run-probes.sh \
  --profile standalone-wine \
  --wine /Users/averyfelts/.metalsharp/runtime/wine/bin/wine \
  --prefix /Users/averyfelts/.metalsharp/prefix-steam \
  --dxmt-runtime /Users/averyfelts/.metalsharp/runtime/wine/lib/dxmt_m12 \
  --results-dir /private/tmp/phase6-semantic-current \
  --semantic-only
```

## Independent render-target logic operations

Metal exposes one logic operation on a render pipeline, while D3D12 permits
independent logic-operation state for each color attachment. The graphics PSO
provider now creates one pipeline variant per render target. Each variant keeps
only its target's color write mask, selects that target's global Metal logic
operation, and the replay path encodes the draw once for each target before
restoring the base pipeline.

The exact two-target readback uses `IndependentBlendEnable=TRUE`, a D32 depth
attachment with depth writes enabled, XOR on target 0, AND on target 1, and
distinct initialized RGBA8 values:

- target 0: `0xaaffff3c` (`clear XOR shader output`);
- target 1: `0x550a0c30` (`clear AND shader output`).

The formerly rejected `logic_op_mrt_independent_variants` PSO case now creates
successfully, and `logic_op_independent_readback=true` is required by
`probe-graphics-pso`. The replay variant is deliberately fail-closed for pixel
shaders with UAV side effects, because repeating a draw would repeat those
side effects; depth/stencil writes use a no-write color pass plus one final
state-only replay, which is exercised by the D32 depth case.

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

The independent render-target logic-operation proof is in the graphics PSO
probe. Its disposable result is:
`/private/tmp/phase6-logic-depth-proof/probe-graphics-pso-standalone-wine.json`.

## Remaining Phase 6 work

The ROV checkpoint does not close Phase 6. Full graphics-stage/topology,
geometry and tessellation breadth, conservative rasterization, programmable
sample positions, barycentrics/view instancing, VRS image/layout breadth,
formats, depth bias, and complete MSAA/readback matrices remain open.
