# DXMT Proton-Parity Roadmap

Created: 2026-05-29
Status: Proposed

This roadmap doubles down on MetalSharp's direct D3D→Metal architecture (the correct path for macOS) while systematically adopting Proton's proven runtime discipline patterns. Every item is grounded in current codebase analysis.

## Why Not Pivot to Proton/Linux

Proton runs Windows PE binaries through Wine on Linux with D3D→Vulkan translation. MetalSharp runs Windows PE binaries through Wine on macOS with D3D→Metal translation. These are the same architectural pattern on different platforms. Proton cannot run on macOS because:
- VKD3D-Proton requires Vulkan extensions MoltenVK doesn't provide
- Proton's Wine fork has no macdrv backend
- Steam Linux Runtime uses Linux namespaces (no macOS equivalent)

MetalSharp's direct D3D→Metal path avoids the double-translation tax (D3D→Vulkan→Metal) and doesn't depend on Apple's closed-source D3DMetal. This is the right architecture. The play is to accelerate DXMT's coverage and mine Proton's ecosystem for patterns.

---

## Phase 1: DXIL Converter Critical Fixes

**Why first:** The DXIL→MSL converter is the #1 blocker for D3D12 games. 75% of shaders fail. Three fundamental issues make most real shaders produce incorrect MSL.

**Impact:** Unblocks every D3D12 game that uses DXIL shaders (SM 6.0+), which is the majority of modern titles.

### 1A. Control Flow Graph Reconstruction — DONE

**Completed:** 2026-05-29. Branch `codex/beta7-dxmt-cohesion` on `aaf2tbz/metalsharp`.

**What was done:**

Parser fixes (`llvm_bitcode.cpp/hpp`):
- Track basic block value IDs in `DECLAREBLOCKS` so PHI block references resolve to block indices
- Store both incoming value AND incoming block in PHI operands (was losing block refs)
- Parse `Switch` instructions (was falling through to `default: break`)

Converter rewrite (`dxil_to_msl.cpp`):
- Build successor/predecessor CFG from branch terminators
- Collect PHI info with (value, predecessor block) pairs
- Pre-declare PHI variables at function scope with typed defaults (float, double, int vector, float vector)
- Emit blocks with labels (`bb0:`, `bb1:`, ...) and `goto`-based control flow
- Conditional `Br` → `if (cond) goto bb_true; else goto bb_false;`
- Unconditional `Br` to next block → fall through (no goto)
- Unconditional `Br` to non-next → `goto bbN`
- Switch → `switch/case/goto` dispatch
- PHI assignments at predecessor block exits before terminator
- Ret/Unreachable handled in CFG emission, not emitInstruction
- Diagnostic trace for unresolved PHI block references

**Syntax validation:** `clang++ -fsyntax-only -Wall -Wextra -Werror` passes on all three changed files.

**Remaining:** Unit tests (Phase 1E) and live shader validation against real DXIL game shaders.

### 1B. Struct Field Extraction Fix — DONE

**Completed:** 2026-05-29. Branch `codex/beta7-dxmt-cohesion` on `aaf2tbz/metalsharp`.

**What was done:**

Value type tracking (`dxil_to_msl.hpp`):
- Added `value_type_ids` parallel vector to `EmitContext` — maps value IDs to their LLVM type IDs
- Populated from: Call instruction results (texture load returns), InsertValue results, PHI results, constants

Struct-aware ExtractValue (`dxil_to_msl.cpp`):
- Looks up aggregate type from `value_type_ids` to distinguish struct vs vector
- ExtractValue on struct field 0: passes through aggregate expression (e.g., `tex.read(coord)` returns float4 — the whole thing IS the value)
- ExtractValue on struct field 1+: emits typed default based on struct field type (0, 0.0f, 0.0)
- ExtractValue on vectors: retains `.x/.y/.z/.w` component suffix (unchanged behavior)
- ExtractValue on unknown types: falls back to current behavior

Struct-aware InsertValue:
- Propagates aggregate type from source to result via `value_type_ids`
- Skips `.x/.y/.z/.w` field writes for struct aggregates (struct field assignment not yet fully supported)

**Impact:** Fixes "member reference on device float" errors from chained ExtractValue on ResRet structs (texture load → extract field 0 → extract component .x). The second ExtractValue now gets a proper float4 instead of a scalar.

**Remaining:** Nested struct field writes in InsertValue, multi-level ExtractValue indices.

### 1C. Groupshared Memory Fix

**Current state:** Alloca handler allocates `thread char storage[256]` (line 3003-3005). This is thread-local, not threadgroup-shared. Any compute shader using shared memory silently uses per-thread copies.

**Files:**
- `dxmt-src/src/airconv/dxil/dxil_to_msl.cpp` lines 2997-3005

**Work:**
- Detect DXIL `groupshared` address space from LLVM alloca metadata
- Emit `threadgroup char storage[N]` for groupshared allocations
- Size the allocation from the DXIL alloca size, not hardcoded 256
- Handle alignment requirements

**Validation:**
- Test compute shader that writes to groupshared, barriers, then reads from another thread
- Verify threadgroup barrier + groupshared interaction

**Milestone:** Compute shaders with groupshared memory produce correct shared-memory MSL.

### 1D. Threadgroup Size from Metadata

**Current state:** Threadgroup size hardcoded to `{1,1,1}` in converter output (line 3384-3386). The caller must override from DXIL metadata.

**Files:**
- `dxmt-src/src/airconv/dxil/dxil_to_msl.cpp` lines 3384-3386
- `dxmt-src/src/airconv/dxil/dxil_container.cpp` (parse entry point metadata)

**Work:**
- Parse `[numthreads(X,Y,Z)]` from DXIL entry point metadata
- Emit correct threadgroup size in MSL compute kernel attribute
- Pass through to `MSLShader::tg_size`

**Milestone:** Compute shaders emit correct threadgroup size without caller override.

### 1E. DXIL Converter Test Infrastructure

**Current state:** Zero unit tests for DXIL path. All 6 conformance probes test DXBC SM 5.0 only.

**Work:**
- Create `dxmt-src/tests/dxil/` directory
- Build HLSL→DXC→DXIL→MSL→metallib pipeline test
- Fixture shaders covering: basic compute, vertex+pixel pair, texture sampling, structured buffer, constant buffer, branch/loop, groupshared
- Add to CI alongside existing DXBC probes
- Regression tests for the May 26 scalar/vector fixes

**Milestone:** 10+ DXIL test shaders compile and produce valid Metal shaders.

---

## Phase 2: Per-Game Prefix Isolation

**Why second:** The shared `prefix-steam` is the biggest Proton pattern gap. One game's VC++ install breaks another's. Registry conflicts between games. DXSetup can't be run safely.

**Impact:** Eliminates the class of bugs where "game A worked until I installed game B."

### 2A. Per-Appid Wine Prefix Creation

**Current state:** All Steam games use `~/.metalsharp/prefix-steam/`. The compatdata doc says per-game prefixes are "future work."

**Files:**
- `metalsharp-repo/app/src-c/runtime/steam_actions.c` (shared prefix launch)
- `metalsharp-repo/app/src-c/runtime/steam.c` (Steam lifecycle)
- `metalsharp-repo/app/src-c/runtime/bottles.c` (bottle prefix contract)
- `metalsharp-repo/docs/runtime/compatdata-architecture.md` line 23

**Work:**
- Create `~/.metalsharp/compatdata/<appid>/pfx/` per Steam game on first launch
- Copy `prefix-steam` as template for new per-game prefixes (Proton's `default_pfx` pattern)
- Wine Steam itself stays in `prefix-steam` (only game processes move to per-game prefixes)
- Update `steam_launch_prefix()` to return per-game path when available
- Mount Steam library drives into per-game prefixes (drive letter mapping)
- Migration: existing `prefix-steam` remains as the Steam client prefix; new launches create per-game

**Milestone:** Two different Steam games can install conflicting VC++ runtimes without breaking each other.

### 2B. Prefix Template with Base Redistributables

**Current state:** MetalSharp detects missing components but doesn't auto-install them. Proton copies base redists into every new prefix.

**Files:**
- `metalsharp-repo/app/src-c/runtime/steam_actions.c` (route readiness)
- `metalsharp-repo/app/src-c/runtime/bottles.c` (component system)

**Work:**
- Create `~/.metalsharp/runtime/redist/` central DLL cache (d3dcompiler_*, d3dx9_*, xinput1_3, xaudio*, vcruntime140, msvcp140, ucrtbase, atl, openal, physx)
- Symlink or copy from central cache into new per-game prefixes (Proton's `builtin_dll_copy` pattern)
- Source DLLs from: Steam CommonRedist, MetalSharp runtime, DXC redistributable
- Component receipts: track what was installed in each prefix

**Milestone:** New per-game prefix is pre-populated with standard redists. Game that needs d3dx9_43.dll finds it without manual install.

### 2C. Redistributable Auto-Install

**Current state:** Component repair requires manual trigger or Runtime Doctor. Proton auto-installs CommonRedist during game install.

**Files:**
- `metalsharp-repo/app/src-c/runtime/setup.c` (runtime setup)
- `metalsharp-repo/app/src-c/runtime/bottles.c` (component repair)

**Work:**
- On first game launch, check component requirements from `mtsp-rules.toml`
- Auto-run silent VC++ / DirectX installers into per-game prefix when missing
- Track install receipts per prefix
- Handle DXSetup.exe, vcredist_x64.exe, vcredist_x86.exe via Wine with /Q silent flags
- Source from Steam CommonRedist or `~/.metalsharp/runtime/redist/`

**Milestone:** Game requiring vcrun2019 + directx_jun2010 auto-installs both on first launch without user intervention.

---

## Phase 3: Compat Config Expansion

**Why third:** With per-game prefixes working and DXIL converter fixed, the next multiplier is game coverage. MetalSharp has 64 game rules. Proton has 200+ hardcoded plus thousands via Steam cloud.

**Impact:** Goes from "works for games we've manually tested" to "works for most D3D11/D3D12 games."

### 3A. Compat Config Flag System

**Current state:** Some per-game fixes remain hardcoded in the C launch route. Proton uses a flag system that Steam can toggle from the cloud.

**Files:**
- `metalsharp-repo/configs/mtsp-rules.toml` (current 64-game rules)
- `metalsharp-repo/app/src-c/runtime/mtsp.c` (rules parser)
- `metalsharp-repo/app/src-c/runtime/steam_actions.c` (hardcoded fixes)

**Work:**
- Extend `mtsp-rules.toml` with flag fields:
  ```toml
  [overrides.1962700]
  pipeline = "m12"
  name = "Subnautica 2"
  flags = ["disable_nanite", "disable_pso_cache", "transcode_movies"]
  gpu_vendor_stubs = ["nvidia"]
  heap_delay_free = false
  preferred_exe = ["Subnautica2.exe"]
  config_patches = [
    { path = "Saved/Config/{platform}/Engine.ini", section = "SystemSettings", keys = { "r.Nanite" = "0" } }
  ]
  launch_args = ["-NoNanite", "-NoShaderPipelineCache"]
  ```
- Migrate hardcoded fixes from C to TOML data
- Eliminate `scripts/setup-*-deps.sh` shell scripts (move logic into TOML + C)

**Milestone:** All per-game behavior is expressed in TOML. Zero game-specific hardcoded logic in C.

### 3B. Proton Game Fix Mining

**Current state:** 64 games in mtsp-rules.toml. Proton has ~200+ game-specific fixes in its Python script.

**Work:**
- Parse Proton's `default_compat_config()` appid→flag mapping
- Cross-reference with MetalSharp's existing rules
- For each Proton game fix, determine:
  - Is it a Linux/Vulkan-specific fix? (skip these)
  - Is it a Wine/DLL/env fix applicable to macOS? (port these)
  - Is it a new game not in MetalSharp's rules? (add entry)
- Categories to port: disablenvapi, heapdelayfree, heapzeromemory, WINEDLLOVERRIDES tweaks, WINE_CPU_TOPOLOGY, launch arg appends
- Categories to skip: PROTON_USE_WINED3D, PROTON_NO_FSYNC, Steam Linux Runtime flags

**Target:** Expand mtsp-rules.toml from 64 to 200+ game entries.

**Milestone:** Top 200 Steam games by player count all have TOML entries (even if just `pipeline = "m12"`).

### 3C. Config File Patching Generalization

**Current state:** Subnautica 2 has hardcoded `write_marked_config_block()` for Engine.ini. No other games get config patches.

**Files:**
- `metalsharp-repo/app/src-c/runtime/steam_actions.c` (launch configuration)

**Work:**
- Generalize to data-driven config patch system in TOML
- Support: INI format, JSON format, XML format
- Marked block replacement (only rewrite between MetalSharp markers)
- Per-game config paths with platform variable substitution
- UE5 Engine.ini, GameUserSettings.ini patterns as templates

**Milestone:** Any game can get INI config patches via TOML without code changes.

---

## Phase 4: Steam Bridge Completion

**Why fourth:** The lsteamclient bridge only forwards 6 Steam API functions. Proton auto-generates hundreds of interface thunks. Broken Steam integration means no Steam Overlay, no achievements, no Workshop, no cloud saves for most games.

**Impact:** Full Steam functionality (overlay, friends, achievements, workshop, cloud saves) for games running through MetalSharp.

### 4A. Vendor Proton lsteamclient Source

**Current state:** `src/steam/build.sh` references `src/steam/proton/lsteamclient/` but the directory doesn't exist. The build infrastructure is scaffolded but the Proton source isn't vendored.

**Files:**
- `metalsharp-repo/src/steam/build.sh` (scaffolded build script)
- `metalsharp-repo/src/steam/bridge/unix_steamclient.c` (hand-written 6-function bridge)

**Work:**
- Vendor Proton's `lsteamclient/` source tree into `src/steam/proton/lsteamclient/`
- Pin to Proton 11.0 branch (matching Wine 11.x baseline)
- Adapt build.sh for macOS: arm64 Unix dylib + x86_64 PE DLL
- Generate `cppISteam*.cpp` interface thunks from Steam API headers
- Build both `steamclient64.dll` (PE) and `lsteamclient.dylib` (macOS native)

**Milestone:** Proton lsteamclient builds on macOS with full Steam API surface.

### 4B. Steam API Integration Testing

**Work:**
- Test ISteamUser (login state), ISteamFriends (overlay), ISteamUserStats (achievements)
- Test ISteamRemoteStorage (cloud saves), ISteamUGC (workshop)
- Test ISteamApps (app ownership), ISteamUtils (overlay toggle)
- Verify Steam callbacks flow correctly through the bridge
- Test with a game that uses Steam API heavily (e.g., a Steamworks SDK example app)

**Milestone:** A Steam game running through MetalSharp shows Steam overlay, records achievements, and syncs cloud saves.

---

## Phase 5: DXIL Converter Expansion

**Why fifth:** After critical fixes (Phase 1) unblock basic games, this phase expands shader coverage to handle more complex D3D12 titles.

### 5A. Texture Dimension Coverage

**Current state:** All textures are `texture2d<float>`. No 1D, 3D, cube, array, or MSAA textures.

**Files:**
- `dxmt-src/src/airconv/dxil/dxil_to_msl.cpp` (texture type emission, sampling, store)

**Work:**
- Detect texture dimension from DXIL resource kind (Texture1D/2D/3D/Cube/1DArray/2DArray/CubeArray/MS)
- Emit correct Metal texture types: `texture1d`, `texture2d`, `texture3d`, `texture_cube`, `texture2d_array`, `texturecube_array`, `texture2d_ms`
- Handle cube map sampling with `sample(cube, float3)`
- Handle array sampling with `sample(array, float2, uint/array_index)`
- Handle MSAA with `read(uint2, uint/sample)`

**Milestone:** Cube map reflections, texture arrays, and 3D volume textures work in DXIL shaders.

### 5B. Wave/Subgroup Operations

**Current state:** Wave operations are advertised via CheckFeatureSupport (OPTIONS1: WaveOps=TRUE, WaveLaneCount=32) but the converter emits nothing for wave intrinsics.

**Work:**
- Map DXIL wave intrinsics to Metal SIMD-group operations:
  - `WaveIsFirstLane` → `simd_is_first()`
  - `WaveGetLaneIndex` → `simd_lane_id`
  - `WaveGetLaneCount` → `simd_lane_count`
  - `WaveActiveAllTrue/AnyTrue` → `simd_all()/simd_any()`
  - `WaveActiveBallot` → `simd_ballot()`
  - `WaveReadLaneFirst/At` → `simd_broadcast()` / `simd_shuffle()`
  - `WaveActiveOp` (sum/bitand/bitor/bitxor/min/max) → `simd_reduce_*()`
  - `WavePrefixOp` → `simd_prefix_inclusive_*()`
  - `QuadReadLaneAt` → `quad_shuffle()`
- Handle quad-wide operations for pixel shaders

**Milestone:** Compute shader using WaveActiveSum and WavePrefixSum produces correct results.

### 5C. Atomic Operation Completeness

**Current state:** Only `atomic_fetch_add_explicit` and `atomic_load_explicit` are implemented. Missing: min, max, and, or, xor, exchange.

**Work:**
- Implement all DXIL AtomicBinOp sub-opcodes: Add, Sub, And, Or, Xor, IMin, IMax, UMin, UMax
- Implement atomic compare-exchange with proper loop pattern
- Handle 64-bit atomics (int64 shader ops are advertised)
- Test with real game compute shaders that use atomic counters

**Milestone:** All DXIL atomic operations produce correct Metal atomic calls.

### 5D. Geometry and Tessellation Shaders

**Current state:** Geometry shader passthrough exists in the command queue (via Metal object+mesh pipeline) but the DXIL converter doesn't handle geometry shaders. Tessellation has 5 `TESS TODO` markers.

**Files:**
- `dxmt-src/src/airconv/dxil/dxil_to_msl.cpp` (no GS/HS/DS in emitFunctionPrologue)
- `dxmt-src/src/airconv/dxbc_converter_ts.cpp` (DXBC tessellation, 5 TODOs)
- `dxmt-src/src/airconv/dxbc_converter_gs.cpp` (DXBC geometry)

**Work:**
- Add GS handling to DXIL converter emitFunctionPrologue
- Add HS/DS handling for tessellation
- Map DXIL geometry shader output to Metal mesh shader thread dispatch
- Map hull/domain shaders to Metal tessellation (post-tessellation vertex shader)

**Milestone:** Game using geometry shaders (e.g., particle systems) renders correctly through DXIL path.

---

## Phase 6: Advanced Features

**Why last:** These features are needed for specific modern games but aren't blocking the majority of titles.

### 6A. Enhanced Barriers

**Current state:** EnhancedBarriersSupported=FALSE in CheckFeatureSupport. `ResourceBarrier` is implemented but `BEGIN/END` split barriers are not.

**Impact:** UE5 games increasingly use enhanced barriers.

### 6B. Stream Output

**Current state:** `SOSetTargets` is empty (line 524). Stream output (transform feedback) is not implemented.

**Impact:** Games using SO for particle systems or GPU-driven rendering.

### 6C. Mesh Shaders / Amplification Shaders

**Current state:** `DispatchMesh` is empty. Mesh/amplification shader kinds fall through to `unknown_main` in the converter. Metal 3 supports mesh shaders via `object` and `mesh` function roles.

**Impact:** Games using mesh shaders (Nanite in UE5, DirectX 12 Ultimate titles).

### 6D. Ray Tracing (DXR)

**Current state:** All ray tracing command list methods are empty. No DXR intrinsics in the converter. Metal 3 supports ray tracing via the `raytracing` namespace.

**Impact:** Games using DXR ray tracing (Cyberpunk 2077, RE4 ray tracing, etc.). Long-term feature.

---

## Success Metrics

| Metric | Current | Phase 1 | Phase 2 | Phase 3 | Phase 4 |
|--------|---------|---------|---------|---------|---------|
| DXIL shaders compiling correctly | ~25% | ~60% | ~60% | ~65% | ~70% |
| Per-game prefix isolation | No | No | Yes | Yes | Yes |
| Game rules in mtsp-rules.toml | 64 | 64 | 64 | 200+ | 200+ |
| Steam API functions bridged | 6 | 6 | 6 | 6 | 100+ |
| Redist auto-install | No | No | Yes | Yes | Yes |
| D3D12 games reaching main menu | 1-2 | 5-10 | 5-10 | 15-20 | 15-20 |

## Dependency Graph

```
Phase 1A (CFG) ──→ Phase 1E (Tests)
Phase 1B (Struct) ─┘
Phase 1C (Groupshared) ─┘
Phase 1D (Threadgroup) ─┘

Phase 2A (Per-game prefix) ──→ Phase 2B (Redist template) ──→ Phase 2C (Auto-install)

Phase 3A (Flag system) ──→ Phase 3B (Proton mining) ──→ Phase 3C (Config patches)

Phase 4A (lsteamclient) ──→ Phase 4B (Testing)

Phase 1E ──→ Phase 5A (Textures) ──→ Phase 5B (Wave ops) ──→ Phase 5C (Atomics) ──→ Phase 5D (GS/Tess)

Phase 5D ──→ Phase 6A (Barriers) ──→ Phase 6B (SO) ──→ Phase 6C (Mesh) ──→ Phase 6D (DXR)
```

Phases 1, 2, 3, and 4 can proceed in parallel with different developers. Phase 5 depends on Phase 1 completion. Phase 6 depends on Phase 5.
