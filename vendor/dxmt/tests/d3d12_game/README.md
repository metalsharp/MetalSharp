# vkd3d_game.exe

`vkd3d_game.exe` is a DX12-only Wine/Metal stress harness for VKD3D. It is meant to
exercise backend surfaces without launching a real game.

The default harness case is a 10-second 3D RGB cube scene with a depth buffer,
ground plane, projected shadow, dynamic vertex updates, indexed draws, and
readback validation. It is intended to stress the render path without requiring
a real game launch.

The harness currently covers:

- device, queue, swapchain, RTV, fence, and present creation
- clear-only present
- sparse vertex buffers using slots 0 and 3
- `DrawInstanced` and `DrawIndexedInstanced`
- SRV/sampler descriptor tables and texture sampling
- dynamic 3D indexed cube rendering with depth, lighting, and shadows
- copy/readback validation using colored-pixel counts and a checksum

`vkd3d_stress_game.exe` is the heavier title-shaped harness. It runs a 5-second
fullscreen splash/movie-style phase before entering a dense 3D scene, then runs
until 15 seconds by default. It deliberately stresses:

- a startup texture containing `STRESS TEST STARTING`
- game-style prerequisite checks for D3D12, DXGI, D3DCompiler, optional DXC,
  optional DXIL, and D3D feature-level 12_0/12_1/12_2 negotiation
- Unreal-style D3D12 capability checks, excluding project/executable-specific
  Unreal checks: feature levels, root signature 1.1, UMA/architecture, GPU
  virtual address size, render/depth/HDR format support, MSAA quality,
  Resource Binding Tier, Resource Heap Tier, Typed UAV format support,
  WaveOps, SM6/SM6.6, native 16-bit shader ops, render pass tier, ray tracing
  tier, VRS tier, mesh shader tier, sampler feedback tier, int64 atomics,
  enhanced barriers, relaxed format casting, vertex element alignment, and GPU
  upload heap support
- a high-density beach scene with animated water, a sunrise disc, sand, a palm
  trunk, irregular palm fronds, a boat hull, sails, and long-shadow lighting
- D3D12 feature queries for resource binding, typed UAV, WaveOps/SM6, and
  mesh/sampler-feedback capability surfaces when exposed by the headers/runtime
- compute PSO creation and `RWTexture2D` UAV writes
- texture upload, SRV descriptor tables, static samplers, and sampling from two
  textures
- dynamic CBV root descriptors
- depth, blend, render-target, UAV, copy-source, and present barriers
- instanced cube draws with per-vertex and per-instance vertex-buffer slots
- dense micro-triangle draw batches to mimic high-triangle/Nanite-style stress
- procedural fullscreen triangle-strip draws using `SV_VertexID`
- splash and scene readback diagnostics

Current hard-failure checkpoint: the beach stress scene compiles and stages,
creates the D3D12 device at feature level 12_0, creates compute/graphics PSOs,
uploads the title texture, builds 13k+ scene vertices, and closes the first
frame command list. The current backend then hits Metal's
`Command encoder released without endEncoding` assertion before present. That
is an intentional high-pressure local repro for encoder lifecycle debugging.

The Unreal-style checks are based on public Epic guidance that UE5 rendering
features such as Lumen, Nanite, Virtual Shadow Maps, and hardware ray tracing
care about DirectX 12, Shader Model 6, SM6.6 atomics for Nanite/VSM-class
paths, hardware ray tracing tiers, and current driver capability reporting.
The corresponding D3D12 queries use Microsoft's documented
`ID3D12Device::CheckFeatureSupport` surfaces.

Build it from an enabled test build:

```sh
meson setup vendor/dxmt/build-metalsharp-x64-tests vendor/dxmt \
  --cross-file vendor/dxmt/build-win64.txt \
  -Denable_tests=true \
  -Dnative_llvm_path=/Volumes/AverySSD/toolchains/clang+llvm-15.0.7-x86_64-apple-darwin21.0 \
  -Dwine_install_path=$HOME/.metalsharp/runtime/wine
meson compile -C vendor/dxmt/build-metalsharp-x64-tests vkd3d_game
meson compile -C vendor/dxmt/build-metalsharp-x64-tests vkd3d_stress_game
```

Run it through the local staging script:

```sh
vendor/dxmt/tests/d3d12_game/run_vkd3d_game.sh
```

Useful variants:

```sh
# Default 10-second RGB cube stress case.
vendor/dxmt/tests/d3d12_game/run_vkd3d_game.sh

# Full focused smoke suite before a shorter cube run.
vendor/dxmt/tests/d3d12_game/run_vkd3d_game.sh --quick-checks --seconds 1

# 15-second title-shaped stress harness.
vendor/dxmt/tests/d3d12_game/run_vkd3d_game.sh --exe vkd3d_stress_game
```

The script stages the PR-built `vkd3d_game.exe`, `d3d12.dll`, `dxgi.dll`,
`dxgi_dxmt.dll`, the deployed DXMT `winemetal.dll`, `d3dcompiler_47.dll`, and a
local Unix-side WineMetal dependency folder under
`$HOME/.metalsharp/tmp/vkd3d_game_run`. For the standalone live harness it also
copies `winemetal.so`, `winemac.so`, and `ntdll.so` beside the executable and
stages the LLVM x86_64 dylibs required by WineMetal (`libc++.1.dylib`,
`libc++abi.1.dylib`, and `libunwind.1.dylib`). It uses
`DXMT_WINEMETAL_UNIXLIB=winemetal.so`, matching the native-override shape that
made the cube path run independently of a game install. It writes:

- `$HOME/.metalsharp/tmp/vkd3d_game_run/vkd3d_game.log`
- `/tmp/winemetal_pe_debug.log` when `DXMT_WINEMETAL_DEBUG=1`

Repository PR CI runs this harness through `tools/ci/vkd3d-check.sh` as the
`VKD3D Check` job. That CI path downloads the released Wine/DXMT runtime inputs,
rebuilds the PR DXMT artifacts with tests enabled, stages the corrected VKD3D
WineMetal layout, builds `vkd3d_game.exe`, and verifies the staged runtime
contract. Set `VKD3D_CHECK_RUN_LIVE=1` to run the quick checks plus the cube scene
locally.

Current observed PR state: draw-bearing command lists are recorded, encoded,
submitted, presented, and read back successfully. The focused smoke suite
passes clear, sparse vertex/index draws, SRV/sampler texture sampling, and the
RGB cube stress case. The final log should end with `=== vkd3d_game.exe PASS ===`
and nonzero `bright`/`chroma` counts.
