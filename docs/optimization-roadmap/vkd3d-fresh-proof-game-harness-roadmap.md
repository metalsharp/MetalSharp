# VKD3D Fresh Proof Game Harness Roadmap
**Updated:** 2026-08-05

> **Status note (2026-08-05):** VKD3D's default backend is now **vkd3d-proton**
> (D3D12 → Vulkan → VKMT MoltenVK → Metal, PR #377); the DXMT stack is the
> `vkd3dBackend=dxmt` rollback. Update the harness target list below
> accordingly: the default VKD3D route deploys `d3d12.dll` + `d3d12core.dll`
> (vkd3d-proton lane), `dxgi.dll` (DXVK lane), and the `nvapi64.dll`/`nvngx.dll`
> stubs (shared dxmt_vkd3d lane) — the DXMT-only files (`dxgi_dxmt.dll`,
> `winemetal.dll`, `winemetal.so`) belong to the rollback lane only.

This roadmap is the first artifact for a new PR branch. It deliberately does not rely on prior probe results, prior proof directories, prior cached shader artifacts, or previous PR evidence. Every pass/fail claim for this PR must be produced again from fresh source inputs, fresh logs, and fresh machine-readable result files.

## Non-negotiable scope

Build a fresh Windows `game.exe`-style proof harness that runs through MetalSharp Wine 11.5 with the real VKD3D route (default vkd3d-proton backend):

- `d3d12.dll` (vkd3d-proton forwarder)
- `d3d12core.dll` (vkd3d-proton implementation)
- `dxgi.dll` (DXVK lane)
- `nvapi64.dll` / `nvngx.dll` (GPU vendor stubs, shared dxmt_vkd3d lane)
- the VKMT MoltenVK runtime (`lib/moltenvk-vkmt`, `VK_ICD_FILENAMES` pinned)
- the selected Wine prefix and runtime layout used by normal games

(The DXMT rollback set — `dxgi_dxmt.dll`, `winemetal.dll`, `winemetal.so` from
`lib/dxmt-vkd3d` — applies only when the harness targets `vkd3dBackend=dxmt`.)

The harness must render an actual loading/game window and must prove, with structured logs and result JSON, that the D3D12/VKD3D pipeline is correct without hidden skips, fallbacks, stale caches, or incorrect DLL routing. It must exercise hundreds of shaders and hundreds of textures when source material is available; one-shader/one-texture smoke coverage is not sufficient for final acceptance because the executable must behave like a real game workload.

## Forward-focused runtime policy

This PR is not constrained by preserving current or old MetalSharp install behavior. If the fresh proof shows the runtime shape must change, the runtime will change. Compatibility gates should target the forward VKD3D architecture this work is creating, not stale behavior from old installs. Existing behavior may be documented as context, but it must not override correctness, exact runtime identity, exact bridge loading, high-volume proof workload, or fresh-proof requirements.

## Backend isolation policy

Any backend used by this work must be built and run on port `9277`. Do not use or disturb the main MetalSharp backend on port `9274`. Any backend-driven proof command must record `METALSHARP_PORT=9277` or equivalent explicit port evidence in its result manifest.

## Fresh artifact policy

- Use a new branch and new PR only.
- Use a new AverySSD result root for all large/generated artifacts.
- Do not treat old captures, old corpora, old shader caches, old screenshots, or old probe outputs as proof.
- All shader inputs used by the harness must be copied into a new corpus manifest with hashes, provenance, and extraction time.
- All generated `.dxil`, `.dxbc`, `.msl`, `.metallib`, cache files, screenshots, and logs must be produced by this branch's tooling during this effort.
- Every gate must emit a machine-readable JSON result and a human-readable summary.

## Target proof root

Large proof output will live outside the repo under:

```text
/Volumes/AverySSD/MetalSharp-SM6-UE-Lab/06-results/in-progress/vkd3d-fresh-proof-game-harness-<timestamp>/
```

The repo will keep only source, scripts, schemas, manifests, and compact summaries.

## Phase 0 — PR hygiene and baseline guard

1. Start from `origin/main` in a clean worktree.
2. Commit this roadmap first.
3. Push a new branch and open a new PR before accumulating implementation commits.
4. Add a disk guard that refuses large proof runs if AverySSD free space would drop below 50 GiB.
5. Add a runtime identity gate that records exact hashes and load paths for `d3d12.dll`, `dxgi.dll`, `dxgi_dxmt.dll`, `winemetal.dll`, and `winemetal.so`.

Exit criteria:

- Clean branch exists.
- Roadmap commit exists.
- New PR can be opened from this branch.
- No previous proof artifacts are referenced as pass evidence.

## Phase 1 — Fresh corpus, SDK, shader, and texture provenance

Create a new compact corpus from genuine Elden Ring and Subnautica 2 shader/PSO/texture inputs, plus fresh engine/vendor inputs gathered specifically for this PR.

Required evidence:

- Source manifest with file hashes, title/source labels, license/provenance notes, extraction time, and acquisition command.
- Local Elden Ring/Subnautica 2 snapshot inventory gate generated during the proof run without launching commercial games. This gate is report/inventory-only unless a later lane binds selected material to presented-frame readback; it must validate captured PSO manifests against real DXBC/MSL/module/report/metallib sidecars and preserve residual missing-sidecar samples without overclaiming runtime execution.
- Actual SM6 and SM5 shader material from the local Unreal Engine repository, including Unreal shader libraries and representative generated shader/debug material where available.
- Real DXIL, DXBC, HLSL, D3D12, and DXGI sample shader inputs from Microsoft sources and/or the local Windows/DirectX SDK/tooling cache.
- Unreal Engine texture/resource payloads from the local Unreal repository or sample content that can be legally used as proof assets.
- Microsoft texture/resource payloads from DirectX samples, DirectXTex/DirectXTK material, SDK sample assets, or local system SDK assets.
- Unity shader and texture inputs if available from Unity sample projects, SRP/URP/HDRP shader libraries, official sample assets, or locally installed Unity material; if unavailable, the manifest must record the attempted sources and why they were unavailable.
- Hundreds-scale shader input coverage where available, with a default target of at least 300 shader files and 300 texture/resource files across game, Unreal, Microsoft, and obtainable Unity sources before final game-harness acceptance.
- Dozens to hundreds of SM5/DXBC graphics paths when available, not just one.
- Dozens to hundreds of SM6/DXIL graphics paths when available, not just one.
- Multiple compute paths from each available source family.
- Hundreds of real texture/resource payloads when available, including game, Unreal, Microsoft, and Unity sources when obtainable.
- Multiple PSO descriptors representative of Unreal-style runtime behavior and multiple representative of Unity-style runtime behavior if Unity inputs are obtainable.

SDK acquisition requirements:

- Fetch or inventory as many relevant current SDKs/toolkits as are legally accessible and useful for this proof, including D3D12 Agility SDK versions, Windows/DirectX SDK samples, DirectXShaderCompiler/DXC, DirectXTex, DirectXTK/DirectXTK12, D3D12 Memory Allocator, PIX/WinPixEventRuntime headers/libs where usable, Microsoft DirectX samples, Apple Metal Shader Converter, and any locally installed Xcode/Metal toolchain pieces.
- Record every SDK version, URL or local path, hash, and compatibility result in the fresh corpus/proof manifest.
- Use SDK diversity to expand compatibility gates rather than preserving current runtime quirks.

Exit criteria:

- Fresh corpus manifest exists and includes game, Unreal, Microsoft, and attempted Unity sourcing.
- SDK inventory manifest exists and maps every SDK/toolkit to the gates it exercises.
- Manifest reports shader, texture/resource, PSO, SM5/DXBC, SM6/DXIL, compute counts, and local game snapshot PSO/sidecar counts; if hundreds-scale targets are not met, it must explain which sources were unavailable and why.
- No shader, texture, cache, SDK, or corpus file is accepted without hash/provenance.

## Phase 2 — Translation accuracy gates

Build fresh translation gates for:

- DXBC parse and lowering.
- DXIL parse and lowering.
- SM5 and SM6 shader profiles.
- Unique non-function callee operands.
- Wave ops.
- Scalar/vector lowering.
- Tessellation policy: either correct translation or explicit unsupported rejection with no false success.
- Nanite-relevant shader capabilities where available.
- MSL generation and Metal compiler validation.

Required evidence:

- Fresh `.msl` output.
- Fresh `.metallib` output.
- Metal compiler diagnostics.
- Structured semantic comparison report.

Exit criteria:

- Translation gate fails hard on skipped, fallback, stale, or unvalidated output.
- Final translation proof covers hundreds of shader inputs when available, and any lower count must be justified by source availability/provenance constraints.

## Phase 3 — Runtime DLL and ABI bootstrap gates

Create a fresh runtime bootstrap executable/gate that proves:

- Correct Wine prefix and route environment.
- Correct native/builtin load behavior for the VKD3D DLL set.
- D3D12 device creation.
- DXGI factory/adapter enumeration.
- DXGI/DXGI_DXMT bootstrap behavior.
- GUID, COM ABI, vtable, and interface querying correctness.
- MetalSharp Wine 11.5 execution path.
- MKVulkan/MoltenVK device reporting/loading status through the Wine runtime's x86_64 Vulkan loader, ICD JSON, `libMoltenVK.dylib`, `winevulkan.so`, and PE `vulkan-1.dll`/`winevulkan.dll` assets.

Exit criteria:

- JSON identifies every loaded runtime module and rejects wrong modules.
- Adapter/device data is reported and validated.
- Vulkan report validates the fresh runtime ICD, opens the x86_64 Vulkan loader, resolves `vkGetInstanceProcAddr`, creates a Vulkan instance, enumerates at least one MoltenVK physical device, and records device name/vendor/type without falling back to host-global stale assets.
- GUID/COM ABI proof validates canonical Direct3D/DXGI interface UUIDs, QueryInterface success, non-null COM vtables, `GetDevice` identity on device-child objects, and `SetPrivateData`/`GetPrivateData` byte roundtrip semantics in both the runtime identity gate and the real presented swapchain game run.
- SM6 DXIL scalar/vector proof makes the presented magenta overlay input-dependent through `dot`, scalar abs/min/max, vector swizzle, saturate, and channel writes; gates validate the source markers, generated MSL scalarized lowering markers, presented shader hashes, and presented-frame center readback.
- SM6 DXIL waveops proof compiles a `cs_6_0` compute PSO, lowers `WaveGetLaneIndex`, `WaveGetLaneCount`, `WaveReadLaneFirst`, `WaveReadLaneAt`, `WaveActiveAnyTrue`, and `WaveActiveAllTrue` to Metal SIMD operations, writes the result through a shader-visible UAV descriptor table texture, validates GPU readback, and copies the stamped wave result into every presented swapchain frame.
- Nanite-style cluster proof dispatches a compute culling pass that writes `D3D12_DRAW_ARGUMENTS` through a UAV, validates the computed args by GPU readback, mirrors the verified args into the CPU-decoded upload indirect-argument buffer used by the current DXMT `ExecuteIndirect` replay path, consumes that buffer with `ExecuteIndirect`, and validates the resulting cluster stamp from presented swapchain-frame readback.
- Texture subresource proof creates a two-slice `Texture2DArray`, uploads distinct RGBA payloads to both subresources, records SRV array/slice descriptor creation as inventory-only metadata, validates both slices by GPU readback, then copies slice 1 into every presented swapchain frame and validates the stamped pixels by presented-frame readback.
- Texture2DArray SRV sampling proof creates an independent two-slice `Texture2DArray`, fills both slices with distinct RGBA payloads, creates a shader-visible Texture2DArray SRV descriptor table plus static sampler, samples slice 1 with `Texture2DArray.Sample` in a `ps_5_0` shader, draws a unique presented-frame stamp, gates unique shader/PSO cache evidence, and validates every stamped pixel by presented-frame readback.

## Phase 4 — Core D3D12 object correctness gates

Extend the harness to prove:

- Command allocators.
- Command lists.
- Command queues.
- Fences and wait/signal ordering.
- Descriptor heaps and descriptor mutation.
- Root signatures.
- Buffers and textures.
- Resources/views/formats.
- Heaps and heap aliasing.
- Barriers, render-pass behavior, and UAV aliasing.

Exit criteria:

- Every object family has at least one positive path and at least one invalid-policy/rejection path where applicable.
- No pass is allowed if the runtime silently falls back or skips work.

## Phase 5 — PSO, cache, prewarm, and binary population gates

Build cold/warm PSO proof for graphics and compute:

- Fresh cold cache population.
- Fresh `.msl` creation.
- Fresh `.metallib` creation.
- Async Metal library load path.
- Warm cache load from generated artifacts.
- PSO cached blob roundtrip or explicit documented unsupported policy if D3D12 semantics cannot be implemented.
- Prewarm manifest generation and consumption.
- Binary archive/cache population and load proof.
- Native Metal archive/prewarm proof that consumes the fresh Wine-run PSO manifests, loads the same fresh shader-cache `.metallib` files, adds render and compute pipeline functions to an `MTLBinaryArchive`, serializes and reloads that archive, and creates render/compute pipeline states asynchronously with `MTLPipelineOptionFailOnBinaryArchiveMiss`.

Exit criteria:

- Cold run creates expected artifacts.
- Warm run loads only validated fresh artifacts.
- Stale, orphaned, missing-reflection, and wrong-hash cache files are rejected.
- Archive/prewarm proof records manifest count, accepted per-hash `.metallib` paths/hashes/sizes under the fresh shader cache, explicit non-rewritten DXIL no-metallib exclusions, archive path/hash/size, add-render/add-compute counts, reload success, and async render/compute callback/success counts.

## Phase 6 — Fresh game.exe window harness

Implement the actual proof executable:

- A loading window.
- A rendered cube or equivalent simple scene.
- A rotating RGB 3D triangle/triangular-prism lane in front of the magenta SM6 triangle, with each visible 3D face using a distinct color and a distinct sampled texture path so the proof demonstrates real textured 3D geometry rather than only 2D texture stamps.
- The rotating 3D triangle lane must use per-face texture payloads from multiple engine/source families when available — Unreal-style, Unity-style if obtainable, Microsoft/DirectX sample, and VS/PS shader-texture paths — and must report exact provenance, hashes, face assignment, presented readback, shader cache, and PSO evidence for each face.
- Real shaders/textures/PSOs from the fresh corpus.
- A high-volume asset exercise mode that loads, binds, translates, caches, and renders or validates hundreds of shaders and textures when available.
- Graphics render thread coverage.
- GPU execution and CPU readback proof.
- Screenshot/readback color verification.
- Structured event log.

Exit criteria:

- The harness opens a real window under MetalSharp Wine.
- It renders deterministically.
- It proves a rotating textured 3D RGB triangle/triangular-prism in front of the magenta SM6 triangle, with distinct per-face colors and engine/source-family texture payloads validated by presented-frame readback.
- It uses fresh game, Unreal, Microsoft, and obtainable Unity shader/texture inputs rather than synthetic-only assets.
- It reports exact loaded/translated/cached/rendered counts and must reach hundreds-scale shader/texture coverage when available.
- It proves GPU computation/readback and draw/color correctness.

## Phase 7 — Engine compatibility simulation

Add Unity/Unreal/Microsoft SDK compatibility scenarios:

- Agility SDK loading path similar to Subnautica 2, across multiple accessible Agility SDK versions where possible.
- Unreal-style root signatures, descriptor tables, PSO creation order, render-thread submission, resource barriers, SM5/SM6 shader families, and Unreal texture/resource payloads.
- Unity-style shader, texture, resource, descriptor, and render-loop lifecycle where Unity inputs are available.
- A textured 3D face-assignment scenario where Unreal/Microsoft/Unity-or-attempted-Unity texture families are sampled on different rotating triangle faces with VS/PS texture-coordinate interpolation and presented readback validation.
- Microsoft DirectX sample and SDK-style DXBC/DXIL/DXGI/D3D12 shader, texture, swapchain, and present paths.
- DXGI swapchain/present/window lifetime and resize behavior.

Exit criteria:

- Compatibility scenarios run through the same runtime route as the game harness.
- Failures are classified by subsystem and block the PR until fixed.

## Phase 8 — Full proof run and PR acceptance

Run the complete fresh gate suite and collect:

- Runtime hash/load-path report.
- Translation accuracy report.
- Core D3D12 object report.
- PSO/cache/prewarm/binary report.
- Windowed game harness screenshot/readback report.
- Logs proving no fallback, no stale cache load, and no incorrect DLL routing.

Exit criteria:

- All required JSON gates pass.
- Human summary maps each goal requirement to fresh evidence.
- Human summary includes high-volume corpus and game-harness counts proving hundreds of shaders/textures were exercised when available.
- Any backend evidence was produced on port `9277`, not `9274`.
- New PR contains only this branch's roadmap, harness, gates, and fixes.
- No live game launch is performed without explicit user approval.
