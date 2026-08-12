# Beta 7: DXMT Cohesion & Rendering Roadmap

Status: **In Progress**
Branch: `codex/beta7-dxmt-cohesion` (off `main` at v0.33.40)
Related: PR #118 (Beta 7 mscompatdb), PR #119 (merged D3D12 bridge), PR #127 (merged DXMT clean install), PR #128 (merged Agility 614)

## Diagnosis

VKD3D D3D12 work changed the DXMT runtime surface as a whole, exposing breakage in the shared D3D11/DXGI/WineMetal path. Two concrete bugs:

1. **M11 BZ PSO Failure** — D3D11 device creates but Metal PSO creation gets garbage vertex buffer indices (2223720704, 4294967295). Uninitialized vertex descriptor state.
2. **VKD3D DXIL SSA Failure** — generated MSL references undeclared SSA values (v197). PHI lowering emits un-materialized expressions.

Plus: DXGI unknown IID crash, feature reporting exceeding capability, and SDK gate too weak.

## Phases

### Phase 7.0: Runtime Cohesion Manifest

One DXMT runtime manifest with hashes for every PE and Unix artifact. No more piecemeal repair.

| Artifact | Path |
|----------|------|
| d3d11.dll | `vendor/dxmt/src/d3d11/` |
| d3d12.dll | `vendor/dxmt/src/d3d12/` |
| dxgi.dll | `vendor/dxmt/src/dxgi/` |
| winemetal.dll | `vendor/dxmt/src/winemetal/` |
| winemetal.so | `vendor/dxmt/src/winemetal/unix/` |
| shader converter | `vendor/dxmt/src/airconv/dxil/` |

### Phase 7.1: D3D11 PSO/Input Layout Fix

**Source:** `vendor/dxmt/src/d3d11/d3d11_input_layout.cpp`, `d3d11_pipeline.cpp`, `d3d11_shader.cpp`
**Reference:** `vendor/dxmt/src/d3d12/d3d12_pipeline_state.cpp` (defensive validation model)

- Zero-init all Metal vertex descriptor structs
- Validate buffer_index < 31 before PSO creation
- Log semantic name, input slot, stride, offset, shader register, Metal buffer index
- Port D3D12 defensive slot model to D3D11

### Phase 7.2: DXIL SSA/PHI Fix

**Source:** `vendor/dxmt/src/airconv/dxil/dxil_to_msl.cpp`, `dxil_to_msl.hpp`

- Add generated-MSL static validator (collect vNNN, fail on undeclared)
- Fix PHI lowering: materialize incoming values topologically
- Replay captured Subnautica 2 shader until clean

### Phase 7.3: D3D12 Feature Reporting

**Source:** `vendor/dxmt/src/d3d12/d3d12_device.cpp`

- Audit CheckFeatureSupport for UE5 queries
- Report SM 6.5 (honest level), not 6.6
- WaveOps reports false until probe suite passes

### Phase 7.4: DXGI Factory/Swapchain

**Source:** `vendor/dxmt/src/dxgi/dxgi_factory.cpp`, `dxgi_output.cpp`

- Resolve IID `c1b6694f-ff09-44a9-b03c-77900a0a1d17`
- Return E_NOINTERFACE for unknown IIDs (no crash)
- Flip-model swapchain support where feasible

### Phase 7.5: SDK Gate Hardening

**Source:** `tools/d3d12-metal-sdk/scripts/`, `tools/d3d12-metal-sdk/contracts/`

- Undeclared SSA = hard fail
- Garbage buffer index = hard fail
- Runtime manifest mismatch = hard fail
- Title-captured shaders in required corpus

### Phase 7.6: Rebuild & Package

- One meson build, one commit, all artifacts
- Manifest generated at build time
- MetalSharp installer verifies before staging

## Validation Matrix

| Probe | Gate |
|-------|------|
| BZ M11 first-frame render | Phase 7.1 |
| S2 VKD3D D3D12 device creation | Phase 7.3 |
| Shader corpus replay (0 undeclared SSA) | Phase 7.2 |
| D3D11 PSO replay (valid buffer indices) | Phase 7.1 |
| DXGI factory probe | Phase 7.4 |
| Swapchain probe | Phase 7.4 |
| Runtime manifest doctor | Phase 7.0 |
| Feature reporting probe | Phase 7.3 |
