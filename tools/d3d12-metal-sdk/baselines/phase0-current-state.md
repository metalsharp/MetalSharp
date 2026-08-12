# Phase 0 Current State Baseline

Captured: 2026-05-24T04:13:56Z

This baseline preserves the local state before SDK implementation work began on PR #119. The primary MetalSharp checkout was dirty, so SDK work was moved to a clean sibling worktree instead of switching branches in place.

## PR Worktree

- Path: `/Users/alexmondello/Dev/metalsharp-pr119-sdk`
- Branch: `codex/d3d12-dxmt-metal-bridge`
- Upstream: `origin/codex/d3d12-dxmt-metal-bridge`
- Starting commit: `18086fd fix(d3d12): enable WaveOps/ROV/ConsRaster, report SM 6.6, fix GUID formatting`
- PR: `https://github.com/aaf2tbz/metalsharp/pull/119`
- PR state: open draft

Recent PR commits at capture:

```text
18086fd fix(d3d12): enable WaveOps/ROV/ConsRaster, report SM 6.6, fix GUID formatting
6ca9fdd fix(dxgi): extend factory to IDXGIFactory7 for UE5 D3D12 adapter probe
21f61a4 feat(d3d12): vendor DXMT D3D12 Metal bridge source (112 commits)
a6491cc chore: remove DXMT submodule — will vendor source directly
26be20b feat(d3d12): vendor DXMT fork with D3D12 Metal bridge as submodule
```

## Dirty Primary MetalSharp Checkout

- Path: `/Users/alexmondello/Dev/metalsharp`
- Branch: `codex/beta7-mscompatdb-routing`
- Dirty files:
  - `app/src-rust/src/mtsp/engine.rs`
  - `app/src-rust/src/mtsp/launcher.rs`
  - `app/src-rust/src/mtsp/recipe.rs`
  - `app/src-rust/src/mtsp/rules.rs`
  - `configs/mtsp-rules.toml`

Diff stat at capture:

```text
app/src-rust/src/mtsp/engine.rs   | 15 ++++-----------
app/src-rust/src/mtsp/launcher.rs | 25 +++++++++++++++++++++++++
app/src-rust/src/mtsp/recipe.rs   |  2 ++
app/src-rust/src/mtsp/rules.rs    |  2 +-
configs/mtsp-rules.toml           |  2 +-
5 files changed, 33 insertions(+), 13 deletions(-)
```

Observed intent in dirty checkout:

- M13/GPTK routing was locally changed away from GPTK DLL deployment and toward builtin Wine/DXMT-style DLL loading.
- Anti-cheat recipes write `steam_appid.txt` into common game directories before launch.
- EXE candidate selection excludes `start_protected` and `easyanticheat`.
- Elden Ring was locally moved from VKD3D to M13 in config and tests.

These changes were not imported into the PR worktree during Phase 0.

## Dirty DXMT Checkout

- Path: `/Volumes/AverySSD/metalsharp/dxmt-src`
- Branch: `codex/d3d12-ordinal-compat`
- Head: `b7240ad feat(d3d12): binding tier 3, crash diagnostics, swapchain present sync, DXGI factory7 stub`
- Dirty files:
  - `src/d3d12/d3d12_device.cpp`
  - `src/dxgi/dxgi_factory.cpp`
  - `src/util/com/com_guid.cpp`

Diff stat at capture:

```text
src/d3d12/d3d12_device.cpp | 20 ++++++++++----------
src/dxgi/dxgi_factory.cpp  | 19 +++++++++++++++++--
src/util/com/com_guid.cpp  | 16 ++++++++--------
3 files changed, 35 insertions(+), 20 deletions(-)
```

Observed intent in dirty DXMT checkout:

- D3D12 feature reporting was being adjusted around WaveOps, ROVs, conservative rasterization, shader model 6.6, and int64 shader ops.
- DXGI factory support was being extended to `IDXGIFactory7`.
- GUID formatting was being fixed to avoid sign-extension in byte formatting.

## Deployed Runtime Hashes

DXMT runtime PE DLLs:

```text
4101ae74d860e88110da7b4438c5ee9a0ba4541947731825c96aede369db3c76  ~/.metalsharp/runtime/wine/lib/dxmt/x86_64-windows/d3d12.dll
4b1325f49d930eb7d12156c38775b43671dc90a377ae7548fff631c0ae9c5528  ~/.metalsharp/runtime/wine/lib/dxmt/x86_64-windows/dxgi.dll
00c82c1381331fd2274c02f353258aa60cdbd6b42dca558fa44ab7d02e82a82a  ~/.metalsharp/runtime/wine/lib/dxmt/x86_64-windows/d3d11.dll
92f04c05cfbe717b3580ef38a5ee5431cd90dc4030ccaa8e6796991b8715df3a  ~/.metalsharp/runtime/wine/lib/dxmt/x86_64-windows/d3d10core.dll
a26151b4bbced27bfcd1d3325c418868546766f7607264c2d0c329e0a33ff2a4  ~/.metalsharp/runtime/wine/lib/dxmt/x86_64-windows/winemetal.dll
cdb05c6737c4b1dc1792f162d098d35357d60ef1f0229cb35c7c09c4de5d048b  ~/.metalsharp/runtime/wine/lib/dxmt/x86_64-unix/winemetal.so
```

Wine builtin replacement hashes:

```text
4101ae74d860e88110da7b4438c5ee9a0ba4541947731825c96aede369db3c76  ~/.metalsharp/runtime/wine/lib/wine/x86_64-windows/d3d12.dll
4b1325f49d930eb7d12156c38775b43671dc90a377ae7548fff631c0ae9c5528  ~/.metalsharp/runtime/wine/lib/wine/x86_64-windows/dxgi.dll
00c82c1381331fd2274c02f353258aa60cdbd6b42dca558fa44ab7d02e82a82a  ~/.metalsharp/runtime/wine/lib/wine/x86_64-windows/d3d11.dll
```

At capture, the DXMT copies and Wine builtin replacement copies of `d3d12.dll`, `dxgi.dll`, and `d3d11.dll` were byte-identical.

## Agility SDK Inputs

- Extracted package root: `/Volumes/AverySSD/metalsharp/metal-api-table/agility-sdk/extracted`
- x64 runtime files:
  - `D3D12Core.dll`
  - `d3d12SDKLayers.dll`
  - `D3D12StateObjectCompiler.dll`
  - `D3D12StateObjectCompiler.exe`
  - `d3dconfig.exe`
- Header root: `/Volumes/AverySSD/metalsharp/metal-api-table/agility-sdk/extracted/build/native/include`

## Mapping Inputs

Reference mapping files available at capture:

```text
/Volumes/AverySSD/metalsharp/metal-api-table/final/AGILITY_SDK_D3D12_TO_METAL_MAP.md
/Volumes/AverySSD/metalsharp/metal-api-table/final/D3D12_TO_METAL_MAP.md
/Volumes/AverySSD/metalsharp/metal-api-table/final/METAL_API_COMPLETE_REFERENCE.md
/Volumes/AverySSD/metalsharp/metal-api-table/final/METAL_ENUMS_MASTER.md
/Volumes/AverySSD/metalsharp/metal-api-table/final/METAL_SELECTORS_MASTER.md
/Volumes/AverySSD/metalsharp/metal-api-table/final/agility_sdk_d3d12_to_metal_map.json
/Volumes/AverySSD/metalsharp/metal-api-table/final/d3d12_to_metal_map.json
```

## Existing DXMT Probes

Existing probe sources and binaries in `/Volumes/AverySSD/metalsharp/dxmt-src/tests`:

```text
probe2/probe2_compute.cpp
probe2/probe2_compute.exe
probe3/probe3_triangle.cpp
probe3/probe3_triangle.exe
probe4/probe4_indexed.cpp
probe4/probe4_indexed.exe
probe5/probe5_depth.cpp
probe5/probe5_depth.exe
probe6/probe6_texture.cpp
probe6/probe6_texture.exe
```

These are useful starting points, but they are not yet a formal SDK conformance suite.

## Subnautica 2 Failure Baseline

Relevant baseline files:

- `~/.metalsharp/compatdata/1962700/logs/launch-1779588911.log`
- `~/.metalsharp/prefix-steam/drive_c/users/alexmondello/AppData/Local/Subnautica2/Saved/Logs/Subnautica2.log`

Key observed lines:

```text
warn:  DXGIFactory: Unknown interface query c1b6694f-ff09-44a9-b03c-77900a0a1d17
info:  D3D12 device created via DXMT Metal backend
LogD3D12RHI: Found D3D12 adapter 0: Apple M4 (VendorId: 1002, DeviceId: 7340, SubSysId: 0000, Revision: 0000
LogD3D12RHI:   Max supported Feature Level 12_1, shader model 6.6, binding tier 3, wave ops supported, atomic64 unsupported
LogD3D12RHI: DirectX Agility SDK runtime not found.
LogD3D12RHI: Chosen D3D12 Adapter Id = 0
LogD3D12RHI: Adapter only supports up to Feature Level 'SM5', requested Feature Level was 'SM6'
LogRHI: RHI D3D12 is not supported on your system.
```

Interpretation:

- The runtime is past basic `D3D12CreateDevice`.
- The blocker is still capability negotiation / Agility-era compatibility, not proof that the game cannot load D3D12 at all.
- The SDK should first reproduce this gate in a non-game probe before changing more game launch behavior.

## Phase 0 Decision

PR #119 currently vendors DXMT source. Phase 0 does not remove or restructure that vendor tree. The safer next step is to build SDK contracts and probes around the current PR state, then decide whether the final PR should keep the vendor tree, use a pinned DXMT source revision, or move to a runtime artifact contract.
