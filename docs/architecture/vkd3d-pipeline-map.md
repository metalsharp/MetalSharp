# VKD3D Pipeline Map
**Updated:** 2026-08-05

VKD3D is the D3D12 route used by the game launcher. Its **default** backend is
**vkd3d-proton**: D3D12 -> vkd3d-proton -> Vulkan -> VKMT MoltenVK -> Metal.
The legacy **DXMT** D3D12 stack remains available as an instant rollback via
the `vkd3dBackend` setting (`vkd3d-proton` default / `dxmt`). The current
MetalSharp tree also contains a native `metalsharp_d3d12` implementation and a
Cocoa/CAMetalLayer viewer path, but those are not the same runtime path that
VKD3D uses for Wine-launched games.

## Runtime Ownership

| Layer | Current owner | Evidence | Status |
| --- | --- | --- | --- |
| Game detection | `app/src-rust/src/mtsp/pe.rs`, `rules.rs` | D3D12 imports select `PipelineId::VKD3D` for 64-bit games. | Present in current project |
| Pipeline definition | `app/src-rust/src/mtsp/engine.rs` | Two VKD3D nodes: `backend: "vkd3d-proton"` (default, `vkd3d_effective_node()`) and the DXMT fallback node selected when `vkd3dBackend=dxmt`. Default deploys vkd3d-proton/DXVK/MoltenVK artifacts and sets `d3d12,d3d12core,dxgi` overrides. | Present in current project |
| Launcher handoff | `app/src-rust/src/mtsp/launcher.rs` | VKD3D routes through `launch_dxmt_metal` (name is legacy), which branches on `node.backend`: vkd3d-proton pins `VK_ICD_FILENAMES` + `VKD3D_SHADER_CACHE_PATH`/`DXVK_STATE_CACHE_PATH`; DXMT sets `DXMT_CONFIG_FILE`/`DXMT_WINEMETAL_UNIXLIB`. | Present in current project |
| Shader/cache routing | `app/src-rust/src/mtsp/shader_cache.rs` | VKD3D uses `vkd3d` and `dxmt-metal12` cache directories. | Present in current project |
| VKD3D default artifact surface | `~/.metalsharp/runtime/wine/lib/{vkd3d-proton,dxvk,moltenvk-vkmt}` | VKD3D loads `d3d12.dll` + `d3d12core.dll` (vkd3d-proton lane), `dxgi.dll` (DXVK lane), and VKMT's patched MoltenVK (`libMoltenVK.dylib` + `MoltenVK_icd.json`). | Present in current project |
| VKD3D DXMT rollback surface | `~/.metalsharp/runtime/wine/lib/dxmt-vkd3d` | Only when `vkd3dBackend=dxmt`; also supplies the shared GPU vendor stubs (`nvapi64.dll`, `nvngx.dll`). | Present in current project |
| Legacy DXMT surface | `~/.metalsharp/runtime/wine/lib/dxmt` | M9/M10/M11 continue to use the known-good legacy DXMT payload. | Present in current project |
| DXMT D3D12 implementation | External DXMT source tree | Conformance branch contains the real DXMT D3D12/DXIL/winemetal work, used only by the VKD3D DXMT rollback lane. | External source tree |
| vkd3d-proton implementation | VKMT vkd3d-proton build | `build-vkmt-win64-filtered` produces the `d3d12.dll` forwarder + `d3d12core.dll` implementation shipped in the bundle. | External source tree |
| Native D3D12 target | `include/metalsharp/D3D12Device.h`, `src/d3d/d3d12/*` | Builds `build/d3d12.dylib` and exposes `D3D12CreateDevice`. | In-tree, smoke-tested |
| Cocoa surface | `src/win32/user32/WindowManager.mm`, `src/dxgi/DXGISwapChain.mm` | Creates NSWindow/CAMetalLayer for the native loader path. | In-tree, not the Wine VKD3D surface |
| Wine VKD3D surface | Default: Vulkan -> VKMT MoltenVK; rollback: DXMT `winemetal.so` | Default VKD3D presents through MoltenVK; the DXMT rollback presents through Wine/winemetal. Neither uses the native `WindowManager` path. | External runtime path |

## VKD3D Launch Flow

1. The PE scanner sees `d3d12.dll` and rules select `PipelineId::VKD3D`.
2. The launcher resolves the game directory and Wine prefix.
3. VKD3D resolves its effective node from the `vkd3dBackend` config
   (`vkd3d_effective_node()`), defaulting to vkd3d-proton.
4. Default VKD3D deploys from the vkd3d-proton/DXVK lanes into the game
   directory: `d3d12.dll`, `d3d12core.dll`, `dxgi.dll`, plus the GPU vendor
   stubs `nvapi64.dll`/`nvngx.dll` (from the shared `dxmt_vkd3d` lane).
   Rollback VKD3D deploys the DXMT set from `lib/dxmt-vkd3d/x86_64-windows`.
5. VKD3D sets `WINEDLLOVERRIDES` so Wine prefers the deployed native DLLs
   (`d3d12,d3d12core,dxgi=n,b`).
6. VKD3D adds the route's unix library paths to `DYLD_FALLBACK_LIBRARY_PATH`
   (default: `lib/wine/x86_64-unix`, `lib/moltenvk-vkmt`; vkd3d-proton itself
   has no unix sidecar — its DLLs live in the windows lane only).
7. VKD3D sets shader/pipeline cache paths under the MetalSharp cache root and —
   for the default backend — pins `VK_ICD_FILENAMES` to the runtime-bundled
   VKMT `MoltenVK_icd.json`.
8. Wine launches the executable without a forced DirectX command-line flag.
   `dx12` and `d3d12` are route aliases for selecting VKD3D, not universal game
   args.
9. vkd3d-proton translates D3D12 to Vulkan; MoltenVK presents on Metal. The
   DXMT rollback instead compiles DXIL/MSL and presents through `winemetal`.

## Current Verification

These checks were run from the repository root:

```sh
cmake --build build --target test_d3d12
cmake --build build --target test_d3d12_entrypoint test_d3d12
./build/tests/test_d3d12
./build/tests/test_d3d12_entrypoint
ctest --test-dir build -R "d3d12|d3d12_entrypoint|phase18|phase19" --output-on-failure
nm -gU build/d3d12.dylib | rg "D3D12CreateDevice|D3D12GetDebugInterface|D3D12SerializeRootSignature"
otool -L build/d3d12.dylib
```

Results:

- `test_d3d12` passed: 50 passed, 0 failed.
- `test_d3d12_entrypoint` passed: 5 passed, 0 failed.
- `ctest` passed `d3d12`, `d3d12_entrypoint`, `phase18`, and `phase19`.
- `build/d3d12.dylib` exports `D3D12CreateDevice`.
- `build/d3d12.dylib` links Metal, Foundation, QuartzCore, AppKit, and
  `libmetalirconverter`.

The external DXMT source tree also rebuilt successfully with:

```sh
ninja -C <dxmt-source>/build src/winemetal/unix/winemetal.so src/d3d12/d3d12.dll
```

The current release-hosted graphics bundle carries five lanes:

- `Graphics/dll/dxmt`: the legacy DXMT surface used by M9/M10/M11.
- `Graphics/dll/dxmt-vkd3d`: the DXMT VKD3D rollback surface (also the source of
  the shared `nvapi64.dll`/`nvngx.dll` GPU vendor stubs), including
  `winemetal.so`, `libc++.1.dylib`, `libc++abi.1.dylib`, and
  `libunwind.1.dylib`.
- `Graphics/dll/vkd3d-proton`: the default VKD3D D3D12 stack
  (`d3d12.dll` forwarder + `d3d12core.dll`, VKMT win64-filtered build).
- `Graphics/dll/dxvk`: shared D3D9/D3D10/D3D11/DXGI surface; `dxgi.dll` for
  the VKD3D route.
- `Graphics/dll/moltenvk-vkmt`: VKMT's patched MoltenVK (`libMoltenVK.dylib`
  + `MoltenVK_icd.json`).

## Completion State

| Area | State | Notes |
| --- | --- | --- |
| VKD3D app routing | Primary/stable | Current project maps D3D12 games to VKD3D before broad directory heuristics, uses VKD3D as the unresolved default, and invokes the backend launcher path. |
| VKD3D backend handoff | Present | The handoff is backend-aware: vkd3d-proton copies vkd3d-proton/DXVK/MoltenVK artifacts and pins `VK_ICD_FILENAMES`; DXMT rollback copies the `dxmt-vkd3d` set. |
| Subnautica-class VKD3D runtime | Demonstrated by local use | This validates the launcher/runtime path, not the native CMake D3D12 dylib. |
| Avery DXMT probes | Strongest external proof (DXMT lane) | `tests/ROADMAP.md` in `dxmt-src` marks probes 2-6 complete, including compute, triangle, indexed draw, depth, and texture sampling. |
| Deployed runtime parity | Split surface | M9/M10/M11 stay on the known-good `dxmt` surface; VKD3D default uses the vkd3d-proton/DXVK/MoltenVK lanes with `dxmt-vkd3d` as rollback. |
| Avery source cleanliness | Needs cleanup | `dxmt-src` has dirty debug/probe changes and notes that prior dirty changes broke Steam launching. |
| Native in-tree D3D12 | Expanded coverage | Smoke, C entrypoint, MSL compute PSO dispatch, and MSL indexed draw tests pass. |
| Native compute PSO | Implemented for MSL/DXBC/DXIL paths | The in-tree native `CreateComputePipelineState` now creates a real Metal compute pipeline when shader bytecode is available. |
| Native indexed draw | Covered by offscreen test | GPU virtual-address lookup now binds real Metal vertex/index buffers and the test executes an indexed draw. |
| Native raytracing/mesh | Stubbed | Advanced D3D12 calls return success or placeholders without full Metal execution. |
| Native Cocoa viewer | Implemented separately | The NSWindow/CAMetalLayer path exists for native-loader presentation; VKD3D Wine games present through MoltenVK (default) or DXMT/winemetal (rollback). |

## Stability Gaps To Close

1. Add a first-class VKD3D runtime verification command in this repo that launches
   a small D3D12 probe through the same `launch_dxmt_metal` environment used by
   games. — **Addressed (Phase 3):** `GET /diagnostics/vkd3d/dry-run?appid=...`
   and `GET /diagnostics/pipeline/dry-run?appid=...&pipeline=vkd3d` report the
   exact env pairs, artifact hashes, and unix sidecars VKD3D would load, using
   the same `steam_pipeline_env_pairs` builder as `launch_dxmt_metal`, without
   launching Steam or the game. The existing `POST /steam/d3d12-runtime-doctor`
   runs the SDK mini-probe suite through that same environment.
2. Add a native Cocoa viewer test target if the goal is to exercise the in-tree
   `metalsharp_d3d12` implementation through CAMetalLayer rather than through
   Wine/winemetal.
3. Expand native D3D12 tests beyond the current graphics/compute coverage:
   texture sampling, depth compare, and swapchain present.

## Practical Conclusion

The current MetalSharp project treats VKD3D as the **vkd3d-proton D3D12 route**
by default (D3D12 -> Vulkan -> VKMT MoltenVK -> Metal) while keeping the DXMT
D3D12 stack available as the `vkd3dBackend=dxmt` rollback. D3D12 PE import
detection selects VKD3D, the backend handoff deploys the vkd3d-proton/DXVK/
MoltenVK runtime by default (the `dxmt-vkd3d` runtime under rollback), and
M9/M10/M11 continue to use the legacy `dxmt` surface that is known to work for
current Steam/Wine titles.

## VKD3D Artifact and Launch Verification (Phase 3)

A reviewer can prove VKD3D loaded the intended artifacts without launching a full
game using the read-only dry-run verifier. It runs through the same environment
builder (`steam_pipeline_env_pairs`) as `launch_dxmt_metal`, so the reported env
pairs and artifact sources are exactly what a real VKD3D launch would use.

- `GET /diagnostics/vkd3d/dry-run?appid=<appid>` — VKD3D-specific dry-run. For the
  default backend it reports the vkd3d-proton/DXVK/MoltenVK artifacts and env
  (`VKD3D_SHADER_CACHE_PATH`, `DXVK_STATE_CACHE_PATH`, `VK_ICD_FILENAMES`); for
  the DXMT rollback it reports the `lib/dxmt-vkd3d/x86_64-unix` sidecars
  (`winemetal.so`, `libc++.1.dylib`, `libc++abi.1.dylib`, `libunwind.1.dylib`)
  and `DXMT_WINEMETAL_UNIXLIB`.
- `GET /diagnostics/pipeline/dry-run?appid=<appid>&pipeline=vkd3d|m11|...` —
  generic pipeline dry-run for comparing lanes.

The dry-run reports, per artifact: resolved source path, presence, sha256, and
size; required artifacts that are missing produce a structured `ok: false` with
a `missing[]` array rather than a silent fallback.

Contract guarantees covered by tests:

- Default VKD3D deploys `d3d12.dll`, `d3d12core.dll`, `dxgi.dll` (DXVK lane),
  and the `nvapi64.dll`/`nvngx.dll` stubs (dxmt_vkd3d lane) from the runtime
  lanes; the DXMT rollback deploys the `lib/dxmt-vkd3d/x86_64-windows` set.
- M11 does **not** deploy `d3d12.dll` and points only at `lib/dxmt`, never
  `lib/dxmt-vkd3d`.
- VKD3D dry-run includes `d3d12.dll`; M11 dry-run does not.
