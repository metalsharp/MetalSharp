# D3D12 full-surface Phase 6 graphics proof

The bounded Phase 6 provider matrix is closed. Exhaustive-feasible Phase 6
completion remains open under
[d3d12-full-surface-phase6-exhaustive-feasible-roadmap.md](d3d12-full-surface-phase6-exhaustive-feasible-roadmap.md).
This proof records the exact positive matrices and the fail-closed boundaries;
it does not promote unsupported combinations.

## Rasterizer-ordered UAV checkpoint

`tools/d3d12-metal-sdk/probes/probe_rov` compiles pinned DXIL pixel shaders
containing `RasterizerOrderedByteAddressBuffer`, `RasterizerOrderedTexture2D<uint>`, and
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
parameter qualifier for each raw, typed, structured, and texture UAV:

```text
range kind=uav ... rasterizer_ordered=1
 device char* buf0 [[buffer(0), raster_order_group(0)]]
 texture2d<uint, access::read_write> tex0 [[texture(0), raster_order_group(0)]]
```

The v2 harness also carries a `RasterizerOrderedBuffer<uint>` typed-buffer
fixture and creates an `R32_UINT` typed UAV view. Under the selected isolated
runtime it now reads back `3` exactly after the same three overlapping
primitives. The expanded matrix additionally proves typed uint/float 2D and
uint 2D-array textures, a D32 depth-enabled pass, and exact rejection of ROV
resources in vertex/compute stages. It also executes the ordered UAV pass with
both D32 depth and D24S8 stencil state. The same run rejects the two-
render-target IndependentBlendEnable ROV/UAV PSO with `0x80004005` before
replay, so no repeated UAV side effect is accepted.

The implementation remains fail-closed for an ROV resource outside the
pixel-UAV provider and for malformed/unsupported resource shapes. Terminal
shader/PSO compilation failures now return `E_FAIL` from D3D12 PSO creation
rather than a non-null object whose later draw would be silently dropped. The
complete declared provider matrix is now independently closed, so
`ROVsSupported=true` is reported only for this raster-order-group provider.

## Binary64 emulation report

The typed DXIL semantic corpus passes all 18 binary64 cases, including basic
arithmetic, bitcast/make/split, dynamic arithmetic, IEEE-754 add/multiply/
divide/remainder/compare matrices, float and integer conversions, unary and
special-value predicates, and FMA. The runtime reports
`DoublePrecisionFloatShaderOps` from the emulation provider. This is exact
bit-level emulation and does not claim native Metal double-precision ALU.

The same semantic run passes native16 arithmetic and math cases through Metal
half operations. `D3D12_OPTIONS4.Native16BitShaderOpsSupported` is promoted
from that provider, while `MSAA64KBAlignedTextureSupported` remains false. The
bounded view-instancing provider also passes the two-view mask/array-slice
matrix with exact red/green `SV_ViewID` output and now reports
`ViewInstancingTier=1`. The dedicated barycentrics probe returns exact
`[128,64,64,255]` for the default perspective `SV_Barycentrics` builtin, which
backs `BarycentricsSupported=true`; noperspective, centroid, sample, and
additional inputs remain outside scope. The command replay probe
also passes a four-pixel/four-sample programmable sample-position pattern with
exact black/red resolve output, backing `ProgrammableSamplePositionsTier=1`.
The remaining minimum-precision report stays conservative pending a dedicated
conversion/rounding matrix; that unverified provider is not advertised by
Phase 6. The semantic reproduction is:

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
restoring the base pipeline. The probe also constructs a UAV-writing pixel
variant and requires independent-logic PSO creation to fail before replay;
this prevents repeated draws from duplicating UAV/ROV side effects.

The exact two-target readback uses `IndependentBlendEnable=TRUE`, a D32 depth
attachment with depth writes enabled, XOR on target 0, AND on target 1, and
distinct initialized RGBA8 values. The companion UAV-writing pixel shader is
rejected at PSO creation with `0x80004005`, proving the replay path cannot
duplicate unordered side effects:

- target 0: `0xaaffff3c` (`clear XOR shader output`);
- target 1: `0x550a0c30` (`clear AND shader output`).

The `logic_op_mrt_independent_variants` PSO case creates successfully, and
`logic_op_independent_readback=true` is required by `probe-graphics-pso`. The
replay variant is deliberately fail-closed for pixel shaders with UAV side
effects, because repeating a draw would repeat those effects; depth/stencil
writes use a no-write color pass plus one final state-only replay, which is
exercised by the D32 depth case.

## Conservative rasterization and graphics state matrix

The conservative-raster probe now runs six cases against the CPU reference:
clipped/partially covered triangles, reversed winding, an offset viewport,
scissor clipping, a D32 depth attachment, and a degenerate triangle. The
8x8 R8G8B8A8 readbacks agree exactly at 176 red pixels. The reference vertex
provider carries viewport coordinates and interpolated depth into the synthetic
point rasterizer; unsupported line/MSAA/VRS/array combinations remain
fail-closed.

## GraphicsCommandList9 topology and depth-bias state

The runtime exposes the Agility 1.619.5 `ID3D12GraphicsCommandList9` methods
`RSSetDepthBias` and `IASetIndexBufferStripCutValue`. Triangle-fan draws are
expanded to an explicit triangle-list index stream during replay. Both the
unindexed and upload-indexed four-vertex fan read back all four `2x2` pixels as
red. A seven-index `0xffffffff` strip with two segments also reads back all
four pixels as red, proving that the restart value is consumed rather than
forwarded as a normal index. GPU-only index buffers use the validated
`ReadBufferRange` snapshot path; malformed or unavailable ranges fail closed.

The dynamic depth-bias lane uses an equal-depth `LESS` test: the pipeline starts
with a D32 target at depth `0.5`, then `RSSetDepthBias(-1, 0, 0)` makes the
full-screen triangle pass and read back exact red. `D3D12_OPTIONS15` reports
triangle-fan and dynamic strip-cut support, and `D3D12_OPTIONS16` reports
dynamic depth-bias support only for these behavior-backed providers.

## GraphicsCommandList8 front/back stencil references

The runtime now exposes the stable Agility 1.619.5
`ID3D12GraphicsCommandList8::OMSetFrontAndBackStencilRef` ABI and carries both
32-bit reference values through command recording and Metal replay. The
behavior probe clears a one-pixel `D24_UNORM_S8_UINT` target to stencil `6`,
sets front/back references to `5/7`, and draws separate front- and back-facing
triangles with `LESS`/`GREATER` tests. Additive red/green output is exactly
`[255,255,0,255]`; using one shared reference would leave either the red or
green contribution absent. The provider is bounded to this exact reference, mask, and operation matrix;
unverified stencil/depth combinations remain fail-closed.

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

## Formats, MSAA, and closure boundary

The resource/views/formats probe passes the declared color, BGRA, float,
integer UAV, vertex/index, sRGB, depth/stencil, typeless, array/mip, cube,
3D, MSAA, placed-resource, castable-view, and footprint cases. The writable
MSAA probe passes the 1x/2x/4x/8x store/load/resolve and graphics DSV cases.
Unsupported format/view combinations stay null or return their documented
validation error. The fresh selected-runtime caps probe reports
`ROVsSupported=true`, `ConservativeRasterizationTier=3`, and the behavior-backed
Options 15/16 topology/depth-bias fields. The legacy regression probe also
returns exact green/blue clear-copy readbacks for D3D11 and D3D10 through the
selected aliases.

## Phase 6 closure boundary

The exhaustive-feasible Phase 6 matrix is closed for ordinary graphics:
graphics PSO state, ROV dimensions/MSAA/order, conservative-raster list/strip/
fan reference coverage, all declared interpolation/evaluation forms,
programmable sample positions, barycentrics, view instancing, VRS image and
layout cases, formats, dynamic depth bias, stencil references, topology and
quadrilateral-line expansion, alpha-to-coverage, forced samples, native and
flattened MSAA, and guarded independent-logic side effects all have exact
fresh-prefix evidence. The authoritative manifest reports zero open legal
classes and zero unclassified legal values. Mesh/work graphs, geometry-stage
breadth, DXR, video, protected resources, DSR, and presentation remain
separate phases.
