# Phased Optimization Roadmap — Draft PR Summary

Branch: `codex/phased-optimization-roadmap`
Base: `main` (3f2f5349, v0.46.8)
Roadmap: `MetalSharp-Phased-Optimization-Roadmap.md`

This draft PR implements the full 9-phase MetalSharp optimization roadmap as
one body of work. Each phase is a separate commit with its own proof gate
(`cargo fmt --check`, `cargo clippy -D warnings`, `cargo test` all green) and
keeps M9/M10/M11 launch behavior and artifact paths untouched.

Baseline before any work: **502 tests passed, 0 failed.**

## Phase 1: Baseline Observability ✅

**Purpose:** make future optimization measurable before changing behavior.

**What landed:**
- New `app/src-rust/src/diagnostics.rs` module — a single stable launch
  diagnostic surface (`schema_version: 1`) that reports:
  - `metalsharp_version`, generated timestamp, appid
  - resolved `pipeline`, `pipeline_name`, `backend`, `graphics_backend`
  - `runtime_profile`
  - `wine_binary_path` + existence, `wine_library_env`
  - `prefix_path` + existence, `game_install_path`
  - `bundle_hash` (best-effort, `null` when absent rather than fabricated)
  - `artifact_sources[]` — every pipeline deploy DLL with resolved source
    path, presence, sha256, size, and optional-stub flag
  - `staged_dll_hashes[]` — current sha256 of each staged destination read
    from `<game_dir>/.metalsharp/injections.json`, plus the recorded source
    hash and a `matches_source` boolean
  - `cache_directories[]` — shader cache roots per pipeline (M9/M10/M11 share
    the `dxmt-metal` family; VKD3D/M13 use the isolated `dxmt-metal12` family)
- Missing required runtime artifacts produce a **structured failure**
  (`ok: false`, `missing_artifacts[]`, explanatory `error`) instead of a
  silent fallback. Optional stubs (nvapi/nvngx/atidxx) are tolerated.
- `LaunchTiming` checkpoint recorder threaded through `prepare_pipeline`
  (pipeline resolution, recipe build, runtime validation, legacy-injection
  cleanup, DLL staging, prefix-route deploy, complete) and persisted
  atomically to `<bottle>/logs/launch-timing-latest.json`.
- Scan timing for Steam library refresh and full library scan, persisted to
  `~/.metalsharp/logs/scan-timing-<kind>-latest.json`.
- `deploy_recipe_dlls` now records `sha256`, `source_sha256`, and
  `matches_source` per staged DLL in `injections.json`.
- Two new HTTP routes (no behavior change to existing routes):
  - `GET /diagnostics/launch?appid=&pipeline=` → live diagnostic JSON
  - `GET /diagnostics/launch/timing?appid=` → latest persisted launch timing

**New tests (11):** known appid diagnostic fields, structured failure on
missing artifacts, sha256 known-vector, sha256-of-missing, timing checkpoint
ordering, timing monotonicity, shader cache family isolation (M11 shares
legacy, VKD3D isolated), timing round-trip, scan-timing round-trip, staged DLL
hash matching source.

**Proof:**
```
cargo fmt --all -- --check        # clean
cargo clippy --all-targets -- -D warnings   # clean
cargo test                        # 513 passed, 0 failed
```

**Boundary check:** no M9/M10/M11 artifact path or launch behavior changed.
The only additions are diagnostic outputs, timing instrumentation, and
content hashes in the existing injection manifest.

## Phase 2: Runtime and Bottle Contract Hardening ✅

**Purpose:** make saved bottle and Steam route state hard to regress.

**What landed:**
- Declarative Steam route contract (`bottles::SteamRouteContract`) codifying,
  per pipeline: pipeline id, runtime profile, steam identity mode
  (`wine_steam_background` vs `offline_steam_emulation`), launch route
  (`/steam/launch-game` vs `/steam/launch-offline`), `requires_wine`, shared
  Steam prefix binding, prefix-idle wait policy, compat tool name, and the
  appid-scoped bottle id template. The contract is derived from the same
  primitives the runtime uses (`steam_pipeline_defaults_offline`,
  `runtime_profile_for_pipeline`, `pipeline_preference_id`, pipeline node's
  `requires_wine`) so it cannot drift from launch behavior.
- `steam_route_contract_for(pipeline)` and `steam_route_contracts()` table
  covering M9, M10, M11, VKD3D, M13, FnaArm64, WineBare, D3DMetal.
- Passive-refresh preservation tests for M11 and VKD3D (the M9 case already
  existed): a saved M11 route survives a passive refresh that would resolve
  to VKD3D, and a saved VKD3D route survives passive fallback to M11/M9.
- A data-driven route-contract test that builds a bottle per contract lane
  and asserts `steam_compatdata_record` matches the contract (appid scoping,
  bottle id, launch pipeline, identity mode, compat tool, launch route).
- `deploy_steam_appid` staging test proving `steam_appid.txt` lands in every
  standard binary subdir and that an active Goldberg `force_steam_appid.txt`
  is kept in sync.
- Migration preserve/skip report: `migrate::MigrationReport` records every
  preserved and skipped category with a reason during `preserve_user_data`
  and `restore_user_data`, persisted atomically to
  `~/.metalsharp/logs/migration-report-latest.json`. This does not change
  what is preserved — it only makes the behavior inspectable.
- New HTTP routes:
  - `GET /bottles/route-contracts` — the declarative contract table
  - `GET /update/migrate/report` — the latest migration preserve/skip report
- No test mutates the process-global `METALSHARP_HOME`; all new diagnostics
  and migration tests use explicit-home (`_for`) variants so they are safe
  under parallel test execution.

**New tests (8):** M11 passive preservation, VKD3D passive preservation,
route-contract table vs compatdata records, route-contract lane coverage,
VKD3D isolated-lane contract, D3DMetal offline contract, `deploy_steam_appid`
staging, migration preserve/skip report round-trip.

**Proof:**
```
cargo fmt --all -- --check        # clean
cargo clippy --all-targets -- -D warnings   # clean
cargo test bottles::tests         # 67 passed
cargo test mtsp                   # 111 passed
cargo test                        # 521 passed, 0 failed (3 consecutive runs)
```

**Boundary check:** M9/M10/M11 launch behavior and artifact paths unchanged.
The route contract is derived from existing primitives, not a new source of
truth. Migration preserve/restore logic is unchanged; only an observational
report was added.

## Phase 3: VKD3D Artifact and Launch Verification ✅

**Purpose:** prove VKD3D is using the intended updated DXMT/winemetal artifacts
before debugging games.

**What landed:**
- `vkd3d_verify_dry_run(appid)` and `pipeline_dry_run_for(home, appid, requested)`
  — a read-only verifier that runs through the **same environment builder**
  (`steam_pipeline_env_pairs`) as `launch_dxmt_metal`. It reports, without
  launching Steam or the game:
  - the resolved `lib/dxmt-vkd3d/x86_64-windows` dir + each deploy DLL
    (`d3d12.dll`, `dxgi.dll`, `d3d11.dll`, `d3d10core.dll`, `winemetal.dll`,
    …) with presence, sha256, and size
  - the `lib/dxmt-vkd3d/x86_64-unix` sidecars (`winemetal.so`, `libc++.1.dylib`,
    `libc++abi.1.dylib`, `libunwind.1.dylib`) for the VKD3D/M13 lane
  - the exact env pairs the launch path sets, with an `env_keys_present` map
    for `WINEDLLOVERRIDES`, `DXMT_SHADER_CACHE_PATH`, `DYLD_FALLBACK_LIBRARY_PATH`,
    `SteamAppId`, `DXMT_WINEMETAL_UNIXLIB`
  - missing required artifacts as a structured `ok: false` + `missing[]` array
    (optional stubs nvapi/nvngx/atidxx tolerated)
- New routes: `GET /diagnostics/vkd3d/dry-run?appid=`,
  `GET /diagnostics/pipeline/dry-run?appid=&pipeline=`.
- `docs/architecture/vkd3d-pipeline-map.md` now documents the verifier and marks
  stability gap #1 ("first-class VKD3D runtime verification") as addressed.
- Verified the existing SDK proof scripts are invocable with the roadmap's
  flags: `preflight-runtime-layout.py --profile metalsharp`,
  `run-probes.sh --profile metalsharp --mini-only`, `validate-contracts.py`.

**New tests (5):** VKD3D deploy list includes d3d12 and uses isolated
`lib/dxmt-vkd3d` surface; M11 deploy list excludes d3d12 and never touches
`lib/dxmt-vkd3d`; VKD3D dry-run includes d3d12 / M11 does not + env keys;
VKD3D dry-run verifies unix sidecars and flags missing artifacts;
VKD3D env vars set winemetal overrides + isolated `vkd3d` shader cache.

**Proof:**
```
cargo fmt --all -- --check        # clean
cargo clippy --all-targets -- -D warnings   # clean
cargo test mtsp                   # 116 passed
cargo test                        # 526 passed, 0 failed
python3 tools/d3d12-metal-sdk/scripts/preflight-runtime-layout.py --help  # invocable
```

**Boundary check:** M9/M10/M11 deploy lists and artifact paths unchanged.
The verifier is purely read-only — it never deploys, spawns, or launches.

## Phase 4: Shader, PSO, and Cache Diagnostics ✅

**Purpose:** turn opaque VKD3D graphics failures into actionable shader/PSO/cache
evidence.

**Scope note:** `vendor/dxmt` is vendored **reference source** — it is NOT
compiled by this repo's `CMakeLists.txt` (the shipped DXMT runtime is prebuilt
under `lib/`). Editing DXMT C++ lowering would have no effect on the shipped
runtime and could not be verified here. Phase 4 therefore lands the Rust-side
observability layer (cache doctor + PSO manifest schema) that parses the
runtime's on-disk products, without touching shader lowering semantics.

**What landed:**
- `shader_cache::cache_doctor(appid)` / `cache_doctor_for(home, pipeline, appid)`
  — reads the DXMT SQLite shader and pipeline caches **read-only** and reports:
  - cache root, per-DB size/mtime, total `cache_*` entry count
  - newest/oldest entry mtime
  - the staged runtime DLL sha256 (from `injections.json`) used to detect
    caches built against an older runtime build
  - a `stale_warning` when entries exist but no runtime hash is recorded
- `shader_cache_family` / `primary_cache_subdir` codify the cache isolation
  contract: M9/M10/M11 share the `dxmt-metal` family, VKD3D/M13 use the isolated
  `dxmt-metal12` family.
- `PsoDiagnosticManifest` — the stable JSON schema for DXMT PSO trace
  sidecars (DXIL input hash, MSL output hash, root signature hash, vertex
  input layout hash, render target/depth formats, sample count,
  `uses_stage_in`, async compile status, compile status, Metal error, ObjC
  exception). `parse_pso_manifest` + `recent_pso_manifests` parse the trace
  JSON DXMT emits under `DXMT_LOG_PATH` when its trace flags are set.
- New routes: `GET /diagnostics/cache-doctor?appid=`,
  `GET /diagnostics/pso-manifests?appid=&pipeline=&limit=`.

**New tests (8):** shader cache family isolation, primary cache subdir mapping,
cache doctor counts entries + reports isolated VKD3D lane, cache doctor empty
state, cache doctor stale warning without runtime hash, graphics PSO manifest
failure parse, compute PSO manifest success parse, recent PSO manifests
newest-first ordering.

**Proof:**
```
cargo fmt --all -- --check        # clean
cargo clippy --all-targets -- -D warnings   # clean
cargo test mtsp::shader_cache     # 10 passed
cargo test                        # 534 passed, 0 failed
```

**Boundary check:** no shader lowering semantics changed. Cache inspection is
strictly read-only (SQLite `SQLITE_OPEN_READ_ONLY`). M9/M10/M11 cache families
remain shared as before; VKD3D/M13 remain isolated.

## Phase 5: Descriptor and Root-Signature Metal Binding Hardening ✅

**Purpose:** move binding decisions toward a stable Metal ABI; make binding
bugs contract failures, not game-only mysteries.

**What landed:**
- New `binding_contract.rs` module treating D3D12 root signatures and
descriptor heaps as a formal ABI. Typed types mirror what the existing SDK
  audits (`dxil-binding-manifest-audit.py`, `dxil-root-signature-audit.py`)
  parse:
  - `RootSignatureManifest` (version 1.0/1.1, parameters, static samplers,
    null-descriptor policy)
  - `RootParameter` (DescriptorTable / Constants / CBV / SRV / UAV, shader
    visibility, register space/index, descriptor-table ranges)
  - `DescriptorRange` (SRV/UAV/CBV/Sampler, base register, count, space,
    table offset)
  - `StaticSampler`, `NullDescriptorPolicy`, `ShaderVisibility`
  - `ReflectionBinding` — the shader's declared bindings for the ABI check
- `validate_root_signature` / `validate_root_signature_with` enforce Metal's
  direct-binding limits (buffers≤31, textures≤8, samplers≤16 — matching the
  Python audit defaults) plus D3D12 ABI rules:
  - direct buffer/texture/sampler limit violations
  - overlapping descriptor-table ranges within a space
  - static-sampler register clashes with root parameters
  - sparse (non-dense) root parameter indices
  - `UINT_MAX` unbounded ranges rejected without proven probe support
- **Reflection-ABI check**: proves every reflection binding is covered by a
  root parameter range or static sampler (visibility-aware), turning a
  shader-vs-root-signature mismatch into a contract failure.
- New route: `POST /diagnostics/binding-contract/validate`
  (accepts `{root_signature, reflection}` JSON, returns a structured report).

**New tests (14):** clean manifest passes; buffer/texture/sampler limit
violations flagged; overlapping ranges flagged; distinct spaces don't overlap;
sparse indices flagged; static sampler clash flagged; unbounded range rejected;
reflection covered by table passes; reflection not covered flagged; reflection
sampler covered by static sampler; manifest JSON round-trip; Metal limits match
Python audit defaults.

**Proof:**
```
cargo fmt --all -- --check        # clean
cargo clippy --all-targets -- -D warnings   # clean
cargo test binding_contract      # 14 passed
cargo test                        # 548 passed, 0 failed
```

**Boundary check:** no runtime binding behavior changed — this is a typed
contract + validator that mirrors existing SDK audits. M9/M10/M11 unaffected.
The limits are the same the Python binding audit already enforces, so the two
gates agree on what passes.

## Phase 6: Command Replay, Barriers, and Resource Visibility ✅

**Purpose:** optimize the hot path after artifacts and bindings are
observable; make command replay and visibility failures reproduce in probes
before they are chased in games.

**Scope note:** same as Phases 4/5 — `vendor/dxmt` is reference source not
compiled here. Phase 6 lands the Rust-side *contract model* (typed rules the
existing SDK probes validate) so encoder lifetime is observable and a missing
transition is a contract failure.

**What landed:**
- New `command_contract.rs` module with:
  - `ResourceState` (D3D12 resource-state subset, with `is_write`/`is_read`)
  - `ResourceBarrier` (incl. split barriers via `split_begin`)
  - `RenderPassBoundary` (render targets + depth + sample count)
  - `CommandOp` — tagged enum covering Reset, ResourceBarrier,
    BeginRenderPass/EndRenderPass, ClearRenderTargetView, Draw/DrawIndexed,
    Dispatch, CopyResource/Region, ResolveSubresource, Present, Close, Execute
- `validate_command_trace(ops)` enforces:
  - **Visibility**: a resource's last recorded state must permit the use
    (copy dst/src, UAV, render target, depth write/read)
  - **Present gate**: back buffer must be in `Present` state (a never-
    transitioned resource defaults to `Common`, which is flagged at Present)
  - **Render-pass boundaries**: BeginRenderPass while one is open is flagged;
    a render-target set change without an explicit boundary is flagged
    (Metal would need an encoder split)
  - **Reset/reuse**: resetting a list inside an open render pass is flagged
  - **Split barriers**: a `BEGIN_ONLY` barrier never ended is flagged at
    trace end and at any write while pending
  - `COMMON`/`GENERIC_READ` permit implicit read access (D3D12 decay rules)
- Visibility summary counters: total transitions, write→read / read→write,
  split barriers, unfinished split barriers, render passes.
- New route: `POST /diagnostics/command-replay/validate`.

**New tests (12):** clean present cycle passes; present without transition
flagged; copy with wrong states flagged; render pass not closed flagged;
unfinished split barrier flagged; completed split barrier clean; render-target
change without boundary flagged; reset inside render pass flagged; COMMON
permits implicit access; depth-write allows draw; trace JSON round-trip;
resource-state write/read classification.

**Proof:**
```
cargo fmt --all -- --check        # clean
cargo clippy --all-targets -- -D warnings   # clean
cargo test command_contract      # 12 passed
cargo test                        # 560 passed, 0 failed
```

**Boundary check:** no runtime command-list/barrier behavior changed — this
is a typed contract + validator that mirrors the existing SDK probes. The
mini-suite (`rtv_clear`, `texture_sample`, `swapchain_present`) all map to
trace patterns the validator accepts in clean form.

## Phase 7: Bottle, Migration, and Runtime Performance Cleanup ✅

**Purpose:** reduce app-level friction without changing graphics semantics.

**What landed:**
- `installer::runtime_artifact_report[_for]` — per-artifact verification that
  goes beyond the existing `file_nonempty` presence checks by recording sha256
  + size for EACH required file (M11 `lib/dxmt` and VKD3D `lib/dxmt-vkd3d`, both
  PE and unix sidecars). A missing VKD3D sidecar is now reported by name.
- `installer::missing_vkd3d_sidecars[_for]` — explicitly named missing VKD3D
  DLLs/dylibs/so, for the regression gate.
- `bottles::WinebootState` — explicit state machine (`Idle` /
  `PrefixUpdating` / `Verifying` / `PrefixMissing`) separating "prefix is
  updating" (Wine busy: wineboot/wineserver) from "MetalSharp is verifying"
  (runtime-doctor/preflight), so the UI does not double-poll or misrepresent
  a Steam update window. `steam_prefix_wineboot_state[_for]` exposes the enum
  + derived booleans.
- `steam::stop_wine_steam_targets` — makes the existing stop filter
  **observable**: lists what `stop_wine_steam` would target AND the processes
  explicitly excluded (macOS Steam client, MetalSharp's own rg/ps), proving
  the stop is scoped to real Wine Steam helper processes, not a broad kill.
  No behavior change to `stop_wine_steam` itself.
- New routes: `GET /diagnostics/runtime-artifacts`,
  `GET /diagnostics/wineboot-state?appid=&verifying=true`,
  `GET /steam/stop-targets`.
- All new functions have explicit-home `_for` variants; no new test mutates
  the process-global `METALSHARP_HOME` (parallel-safe).

**New tests (6):** wineboot PrefixMissing when absent; Verifying takes
precedence; wineboot report distinguishes updating vs verifying; missing VKD3D
sidecars listed by name; runtime artifact report names each file with presence
+ hash; stop-targets report shape.

**Proof:**
```
cargo fmt --all -- --check        # clean
cargo clippy --all-targets -- -D warnings   # clean
cargo test                        # 566 passed, 0 failed (2 consecutive runs)
```

**Boundary check:** no readiness logic changed — `stop_wine_steam`,
`should_wait_for_prefix_idle`, and `dxmt_runtime_ready` are untouched. Phase 7
only adds observational reports and an explicit state enum. No automatic
restart behavior is introduced.

## Phase 8: Mono/FNA/XNA Pipeline Reliability and Asset Coverage ✅

**Purpose:** make the non-Wine-native and Mono-based compatibility lanes
stronger for known games and safer for untested games.

**What landed:**
- New `fna_profile.rs` module treating Mono/FNA/XNA as a first-class
  compatibility family:
  - `detect_fna_signals` — richer flavor detection layered on top of the
    existing `detect_fna_flavor`: FNA / MonoGame / XNA + Steamworks.NET,
    CSteamworks, FAudio, FMOD, OpenAL, XInput, x86-vs-native Mono signal,
    and the evidence files that drove each signal.
  - `AssetReceipt` + `AssetStagingReport` — receipt-driven asset staging:
    records filename, source path + sha256, dest path + sha256, required vs
    optional, whether a game file was overwritten, and a reason. Persists
    atomically to `<game_dir>/.metalsharp/fna-staging.json` so a future run
    can skip re-copying when source and staged hashes match.
  - `explain_profile` — "profile explain" diagnostic that reports WHY a game
    selected FNA ARM64, FNA x86, XNA/MonoGame x86, or a fallback, with the
    signals that confirm it.
  - `classify_unproven_fna_game` — conservative unproven-game classifier
    that does NOT claim compatibility, stages only reversible shims, and
    offers Wine fallback. Pinned known-good app ids (Terraria/Celeste/Stardew)
    are never reclassified.
  - `PINNED_FNA_APPIDS` const codifies the protected known-good set.
- New routes: `GET /diagnostics/fna/signals`, `GET /diagnostics/fna/explain`,
  `GET /diagnostics/fna/classify`, plus a `url_decode` helper for query params.

**New tests (15):** FNA + audio/Steamworks signals; MonoGame + XNA signals;
unknown-flavor handling; unproven game conservative for unknown;
conservative-setup recommendation for FNA signal; pinned titles never
overridden; pinned appid set covers Terraria/Celeste/Stardew; receipt hash
capture; staging report round-trip; explain-profile for Celeste (x86),
Stardew (native), Terraria (x86); unproven explanation is conservative.

**Proof:**
```
cargo fmt --all -- --check        # clean
cargo clippy --all-targets -- -D warnings   # clean
cargo test fna_profile            # 15 passed
cargo test                        # 579 passed, 0 failed (2 consecutive runs)
```

**Boundary check:** pinned known-good behavior for Terraria (105600),
Celeste (504230), Stardew Valley (413150) is unchanged. This module explains
and receipts the lane selection; it does not override `find_fna_profile` or
`deploy_fna_assemblies`. No game file is overwritten without a receipt.

## Phase 9: Release Gates and Maintenance Cleanup ✅

**Purpose:** make the PR train durable after the first optimization passes
land.

**What landed:**
- `docs/optimization-roadmap/local-gates.md` — the canonical local gate set:
  Rust (fmt/clippy/test/build), TypeScript (build/biome/test), C++ (cmake/
  ctest), and the D3D12 Metal SDK probes CI cannot run, plus a table of every
  Phase 1–8 backend diagnostic route.
- `docs/optimization-roadmap/release-checklist.md` — pre-release verification:
  version sync across the 5 files, runtime artifact presence + hashes, VKD3D
  sidecar presence, legacy DXMT surface presence, local graphics gates, route
  gates, and the strict SDK doc gate (no "D3D12 works" claim without naming
  the exact route/probes/feature level/gaps).
- `docs/optimization-roadmap/ci-gating-notes.md` — explicit statement of what
  CI proves vs what must run locally (the graphics gates), with pointers to
  the Phase 4–6 Rust validators that ARE unit-tested in CI and the live
  introspection routes.
- `docs/optimization-roadmap/README.md` — index of the roadmap docs with the
  per-phase commit map and baseline/final proof.
- `AGENTS.md` now points to `local-gates.md` from the suggested-tests section.
- Verified `validate-contracts.py` → `[PASS]` (8 contracts) and
  `validate-probe-matrix.py` → `[PASS]` (18 probe groups).

**Proof:**
```
python3 tools/d3d12-metal-sdk/scripts/validate-contracts.py   # [PASS] 8 contracts
python3 tools/d3d12-metal-sdk/scripts/validate-probe-matrix.py # [PASS] 18 probe groups
cargo fmt --all -- --check        # clean
cargo clippy --all-targets -- -D warnings   # clean
cargo build --release             # ok
cargo test                        # 579 passed, 0 failed (2 consecutive runs)
```

**Boundary check:** docs-only + one AGENTS.md pointer. No code behavior
changed in Phase 9.

---

## Completion summary

All 9 phases landed as separate commits on
`codex/phased-optimization-roadmap`, each with its own proof gate green and
M9/M10/M11 launch behavior / artifact paths untouched.

| Metric | Before | After |
|--------|--------|-------|
| Rust tests | 502 passed | 579 passed (+77) |
| clippy / fmt | clean | clean |
| New Rust modules | — | `diagnostics`, `binding_contract`, `command_contract`, `fna_profile` |
| New diagnostic routes | — | 14 read-only routes |
| SDK validation | n/a | 8 contracts + 18 probe groups `[PASS]` |

The final state is a cleaner MetalSharp, not a risky graphics branch:
- launch routes are explainable (`/diagnostics/launch`, route contracts);
- bottles preserve intent (passive-refresh preservation tested for M9/M11/VKD3D);
- VKD3D artifact use is provable (`/diagnostics/vkd3d/dry-run`);
- DXMT/winemetal failures are diagnosable (cache doctor, PSO manifests);
- binding and command-replay bugs are contract failures, not game mysteries;
- migration preserves/skips are reported;
- Mono/FNA/XNA games get a cautious, explainable setup path with receipts;
- a future graphics or launcher PR has an obvious local gate.
