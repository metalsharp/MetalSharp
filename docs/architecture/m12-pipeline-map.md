# M12 Pipeline Map
**Updated:** 2026-07-08


Last verified: 2026-06-13.

M12 is the D3D12 -> DXMT -> Metal path used by the game launcher. The current
MetalSharp tree also contains a native `metalsharp_d3d12` implementation and a
Cocoa/CAMetalLayer viewer path, but those are not the same runtime path that M12
uses for Wine-launched games.

## Runtime Ownership

| Layer | Current owner | Evidence | Status |
| --- | --- | --- | --- |
| Game detection | `app/src-c/runtime/steam_actions.c`, `mtsp.c` | D3D12 imports and rules select M12 for compatible 64-bit games. | Present in current project |
| Pipeline definition | `app/src-c/runtime/mtsp.c`, `steam_actions.c` | M12 is named `D3D12 -> Metal via DXMT`, deploys isolated DXMT DLLs, and sets D3D12/DXGI/D3D11 overrides. | Present in current project |
| Launcher handoff | `app/src-c/runtime/steam_actions.c` | The C launch route copies DLLs into the game directory and sets Wine/DYLD/cache env. | Present in current project |
| Shader/cache routing | `app/src-c/runtime/steam_actions.c` | M12 uses isolated `m12` shader and pipeline cache directories. | Present in current project |
| M12 artifact surface | `~/.metalsharp/runtime/wine/lib/dxmt_m12` | M12 loads the updated D3D12/DXGI/winemetal payload from the isolated `dxmt_m12` directory. | Present in current project |
| Legacy DXMT surface | `~/.metalsharp/runtime/wine/lib/dxmt` | M9/M10/M11 continue to use the known-good legacy DXMT payload. | Present in current project |
| DXMT D3D12 implementation | External DXMT source tree | Conformance branch contains the real DXMT D3D12/DXIL/winemetal work used by M12 runtime DLLs. | External source tree |
| Native D3D12 target | `include/metalsharp/D3D12Device.h`, `src/d3d/d3d12/*` | Builds `build/d3d12.dylib` and exposes `D3D12CreateDevice`. | In-tree, smoke-tested |
| Cocoa surface | `src/win32/user32/WindowManager.mm`, `src/dxgi/DXGISwapChain.mm` | Creates NSWindow/CAMetalLayer for the native loader path. | In-tree, not the Wine M12 surface |
| Wine M12 surface | DXMT `winemetal.so` plus Wine/macOS windowing | DXMT presents through Wine/winemetal, not through the native `WindowManager` path. | External runtime path |

## M12 Launch Flow

1. The PE scanner sees `d3d12.dll` and rules select M12.
2. The launcher resolves the game directory and Wine prefix.
3. M12 deploys DXMT PE DLLs from `lib/dxmt_m12/x86_64-windows` into the game directory:
   `d3d12.dll`, `d3d11.dll`, `dxgi.dll`, `d3d10core.dll`, and `winemetal.dll`.
4. M12 sets `WINEDLLOVERRIDES` so Wine prefers the deployed native DXMT DLLs.
5. M12 adds `lib/dxmt_m12/x86_64-unix` and Wine unix library paths to `DYLD_FALLBACK_LIBRARY_PATH`.
6. M12 sets shader and pipeline cache paths under the MetalSharp cache root.
7. Wine launches the executable without a forced DirectX command-line flag. `dx12` and `d3d12` are route aliases for selecting M12, not universal game args.
8. DXMT handles D3D12/DXGI calls, compiles DXIL/MSL work, sends commands through
   `winemetal`, and presents through the Wine/macOS surface.

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

The current release-hosted graphics bundle contains two DXMT surfaces:

- `Graphics/dll/dxmt`: the 0.46.5 legacy surface used by M9/M10/M11.
- `Graphics/dll/dxmt-m12`: the updated M12 surface used only by M12, including
  `winemetal.so`, `libc++.1.dylib`, `libc++abi.1.dylib`, and
  `libunwind.1.dylib`.

## Completion State

| Area | State | Notes |
| --- | --- | --- |
| M12 app routing | Primary/stable | Current project maps D3D12 games to M12 before broad directory heuristics, uses M12 as the unresolved default, and invokes the backend launcher path. |
| M12 backend handoff | Present | The handoff copies isolated M12 DXMT DLLs, configures Wine/DYLD env, cache env, and launch args. |
| Subnautica-class M12 runtime | Demonstrated by local use | This validates the launcher/runtime path, not the native CMake D3D12 dylib. |
| Avery DXMT probes | Strongest external proof | `tests/ROADMAP.md` in `dxmt-src` marks probes 2-6 complete, including compute, triangle, indexed draw, depth, and texture sampling. |
| Deployed runtime parity | Split surface | M9/M10/M11 stay on the known-good `dxmt` surface while M12 uses the updated release-hosted `dxmt-m12` surface. |
| Avery source cleanliness | Needs cleanup | `dxmt-src` has dirty debug/probe changes and notes that prior dirty changes broke Steam launching. |
| Native in-tree D3D12 | Expanded coverage | Smoke, C entrypoint, MSL compute PSO dispatch, and MSL indexed draw tests pass. |
| Native compute PSO | Implemented for MSL/DXBC/DXIL paths | The in-tree native `CreateComputePipelineState` now creates a real Metal compute pipeline when shader bytecode is available. |
| Native indexed draw | Covered by offscreen test | GPU virtual-address lookup now binds real Metal vertex/index buffers and the test executes an indexed draw. |
| Native raytracing/mesh | Stubbed | Advanced D3D12 calls return success or placeholders without full Metal execution. |
| Native Cocoa viewer | Implemented separately | The NSWindow/CAMetalLayer path exists for native-loader presentation, but M12 Wine games present through DXMT/winemetal. |

## Stability Gaps To Close

1. Add a first-class M12 runtime verification command in this repo that launches
   a small D3D12 probe through the same C launch environment used by
   games. — **Addressed (Phase 3):** `GET /diagnostics/m12/dry-run?appid=...`
   and `GET /diagnostics/pipeline/dry-run?appid=...&pipeline=m12` report the
   exact env pairs, artifact hashes, and unix sidecars M12 would load, using
   the same route path and cache builders as a real C backend launch, without
   launching Steam or the game. The existing `POST /steam/d3d12-runtime-doctor`
   runs the SDK mini-probe suite through that same environment.
2. Add a native Cocoa viewer test target if the goal is to exercise the in-tree
   `metalsharp_d3d12` implementation through CAMetalLayer rather than through
   Wine/winemetal.
3. Expand native D3D12 tests beyond the current graphics/compute coverage:
   texture sampling, depth compare, and swapchain present.

## Practical Conclusion

The current MetalSharp project treats M12 as the D3D12 DXMT route while keeping
older DXMT routes isolated. D3D12 PE import detection selects M12, the backend
handoff deploys the `dxmt_m12` runtime, and M9/M10/M11 continue to use the
legacy `dxmt` surface that is known to work for current Steam/Wine titles.

## M12 Artifact and Launch Verification (Phase 3)

A reviewer can prove M12 loaded the intended artifacts without launching a full
game using the read-only dry-run verifier. It runs through the same environment
builders (`set_route_paths` and `set_launch_cache_env`) used by the C launch route, so the reported env
pairs and artifact sources are exactly what a real M12 launch would use.

- `GET /diagnostics/m12/dry-run?appid=<appid>` — M12-specific dry-run including
  the `lib/dxmt_m12/x86_64-unix` sidecars (`winemetal.so`, `libc++.1.dylib`,
  `libc++abi.1.dylib`, `libunwind.1.dylib`).
- `GET /diagnostics/pipeline/dry-run?appid=<appid>&pipeline=m12|m11|...` —
  generic pipeline dry-run for comparing lanes.

The dry-run reports, per artifact: resolved source path, presence, sha256, and
size; required artifacts that are missing produce a structured `ok: false` with
a `missing[]` array rather than a silent fallback. Env keys verified present:
`WINEDLLOVERRIDES` (winemetal overrides), `DXMT_SHADER_CACHE_PATH` (isolated
`m12` lane), `DYLD_FALLBACK_LIBRARY_PATH`/`LD_LIBRARY_PATH`, `SteamAppId`, and
`DXMT_WINEMETAL_UNIXLIB`.

Contract guarantees covered by tests:

- M12 deploys `d3d12.dll`, `dxgi.dll`, `d3d11.dll`, `d3d10core.dll`,
  `winemetal.dll` from `lib/dxmt_m12/x86_64-windows`.
- M11 does **not** deploy `d3d12.dll` and points only at `lib/dxmt`, never
  `lib/dxmt_m12`.
- M12 dry-run includes `d3d12.dll`; M11 dry-run does not.
