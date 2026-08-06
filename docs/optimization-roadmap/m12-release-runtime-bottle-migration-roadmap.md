# M12 Release Runtime, Bottle Save, Migration, and Launch Shape Roadmap

Date: 2026-06-29  
Scope: PR #230 continuation / MetalSharp M12 release runtime update

> **Status note (2026-08-05):** Most of this roadmap's "current state" claims
> are now resolved differently than planned. M12's shipped shape is the
> **vkd3d-proton default** (PR #377): M12 uses `lib/vkd3d-proton` +
> `lib/dxvk` (dxgi) + `lib/moltenvk-vkmt` by default; `lib/dxmt_m12` is the
> `m12Backend=dxmt` rollback lane (still supplying the shared nvapi/nvngx
> stubs). EAC ("Offline EAC Mode") is removed from setup; compatdata
> preserve/restore is gone from migration (still written at launch);
> `DXMT_LOG_PATH` is dev-gated only. The release bundle carries five graphics
> lanes (dxmt, dxmt_m12, vkd3d-proton, dxvk, moltenvk-vkmt), and
> `WINEMSYNC` is config-driven (msync toggle). Treat requirements below as
> the historical DXMT-shape plan; the vkd3d-proton shape supersedes them.

## Purpose

Make the next MetalSharp update correctly ship the isolated PR #230 M12 DXMT runtime shape for new installs and update migrations, while keeping M11 and M12 separated, removing stale compatdata and current anti-cheat/EAC behavior, and ensuring every M12 game uses the updated PR #230 launch/prepare contract.

## Non-negotiable requirements

- M11 must use `runtime/wine/lib/dxmt`.
- M12 must use `runtime/wine/lib/dxmt_m12`. — **superseded:** default M12 uses `lib/vkd3d-proton` + `lib/dxvk` + `lib/moltenvk-vkmt`; `dxmt_m12` is the DXMT rollback lane.
- M12 must use the PR #230/Elden-proven launch shape globally, not one-off per-game hacks.
- Bottle saves/switches between M11 and M12 must prepare and verify the selected runtime lane.
- New installs and migration wizard flows must install/update all required runtime material.
- `metalsharp-runtime.tar.zst` must include the current built backend because installer/migration consume `runtime/metalsharp-backend` from the runtime tarball.
- Compatdata should be removed/deprecated and no longer preserved/restored/written.
- Current anti-cheat/EAC/mscompatdb implementation should be removed/disabled, not repaired.
- Runtime graphics logs must be default-off; developer controls may enable DXMT-style logs.

## Findings from release/build investigation

### Release CI bundle flow

For a tag release, `.github/workflows/release.yml` currently does the following:

1. Builds the Rust backend.
2. Builds host runtime ABI assets.
3. Downloads current split bundles from the `bundles` release.
4. Extracts `metalsharp-runtime.tar.zst` to get Wine build tooling for the M12 DXMT build.
5. Builds/stages M12 DXMT runtime into `dist/dxmt_m12`.
6. Runs `tools/dmg/create-bundles.sh`, which:
   - downloads/validates split bundles,
   - repairs `metalsharp-graphics-dll.tar.zst` with `METALSHARP_DXMT_M12_ROOT`,
   - repairs `metalsharp-runtime.tar.zst` with current `runtime/metalsharp-backend` and host ABI,
   - verifies bundles,
   - writes `dist/bundles/metalsharp-bundle-manifest.tsv`.
7. Builds developer SDK from repaired runtime + repaired graphics bundles.
8. Publishes runtime + developer SDK + manifest to the `bundles` release.
9. Builds the DMG with embedded bundles.
10. Verifies the mounted DMG contains backend, host runtime, updater scripts, and all required bundles.

### Backend in runtime tarball is required

`metalsharp-runtime.tar.zst` must contain:

```text
runtime/wine/...
runtime/host/manifest.json
runtime/host/HostRuntimeABI.h
runtime/host/libmetalsharp_host_runtime.dylib
runtime/metalsharp-backend
runtime/wine/lib/metalsharp/x86_64-windows/metalsharp_ntdll_hook.dll
```

Reasons:

- `tools/dmg/repair-runtime-bundle.py` deliberately injects current backend + host ABI into the runtime tarball.
- `tools/bundles/verify-bundles.sh` verifies `runtime/metalsharp-backend` exists inside the tarball.
- `tools/dmg/verify-dmg-runtime-assets.sh` requires the runtime bundle and backend resource.
- `app/src-rust/src/installer.rs` extracts the runtime tarball and installs/verifies `runtime/metalsharp-backend` under `~/.metalsharp/runtime/metalsharp-backend`.
- Migration invokes installer flow after deleting stale runtime state, so update installs also depend on the tarball backend copy.

The backend is also packaged directly in the DMG as `Contents/Resources/runtime/metalsharp-backend`, but that does not eliminate the need for the tarball copy.

## Current conflicts to resolve

### Compatdata still exists

Current code still writes/preserves/restores:

```text
~/.metalsharp/compatdata/{appid}/metalsharp-compatdata.json
~/.metalsharp/compatdata/{appid}/logs/...
```

There are also endpoints/tests around `/steam/compatdata` and `SteamCompatdataRecord`. This conflicts with the requirement that compatdata is no longer present.

### EAC/anti-cheat still exists

Current setup still includes an `Offline EAC Mode` install step and bundle verification still expects `assets/eac-toggle/x86_64-windows/_winhttp.dll`. Rules still include `anticheat = "eac_eos"` for some games. This conflicts with removing the current anti-cheat/EAC implementation.

### Logging defaults are not clean

`DXMT_LOG_PATH` is currently added by default for DXMT routes through cache env generation. Subnautica 2 M12 also has hardcoded trace/debug env defaults. This conflicts with the default-off graphics logging requirement.

### Bottle save/runtime prepare is incomplete

Current M12 work started in this area, but the desired model must be broader:

- M11 save must ensure/verify the legacy `dxmt` lane.
- M12 save must ensure/verify the isolated `dxmt_m12` lane.
- Switching between M11 and M12 must run the same prepare path launch uses.
- Shared M12 runtime readiness must not be conflated with game-local D3D12 Agility readiness.

## Target runtime and launch shape

### Confirmed-good M12 release material

The final approved Elden Ring proof rendered only after rebuilding/staging the M12 DXMT runtime from last-known-good source commit:

```text
a24464357fc0cb09ba794330d89d7dd6df9e2140
```

The release graphics bundle and developer SDK must contain that M12 runtime material, not a later unverified DXMT build. Release CI must consume and verify the prebuilt release bundles; it must not build or repair runtime/graphics/assets bundles itself. The only bundle artifact release CI may generate is the developer SDK, built from the verified release bundles. The proofed hashes are:

```text
d3d10core.dll  e6647486489473800a85e5ca8dff94e0beec63847138c72d9145297dd97de3c1
d3d11.dll      04f7573de3bdb6953b3df3b4521b8e8c9de2c1d81f62924eec4c9a0f1761471f
d3d12.dll      71b1defeb4ef0b6e7a2bda03f4699f77ed9ac8ebaecf0d93d84e258332083793
dxgi.dll       482ce5711966c24c994ec3939270518f3ebd45d76f50e8a0ae06f2233f13b300
dxgi_dxmt.dll  94c1d253e77f0b5a2a144e170b43d6256bf663a331598352e29c9cfff16e08ec
winemetal.dll  22b90526bd5f9f4cf87770549415d81d5098a3e73a32268c735d95cc2cf3e002
winemetal.so   031dd0030ca0f188fb9d872e9d755a1ea704b97c292ce258cfd376cff92e498b
nvapi64.dll    5254ddf867b89e77b2b341a56541112d0fadd106714c0a6280a07c3a17b3f4e0
nvngx.dll      322bb63e93b83aa0585a6b8ba7dd5cc73f895cfea0a31b210aa50bac1b09375b
```

### M11 lane

M11 uses:

```text
runtime/wine/lib/dxmt/x86_64-windows
runtime/wine/lib/dxmt/x86_64-unix
```

Expected M11 route traits:

```text
WINEDLLPATH includes lib/dxmt/x86_64-windows
DYLD_LIBRARY_PATH / fallback include lib/dxmt/x86_64-unix
WINEDLLOVERRIDES=winemetal,dxgi,d3d11,d3d10core=n,b;gameoverlayrenderer,gameoverlayrenderer64=d
No dxmt_m12 paths
No d3d12.dll deployment requirement for M11
```

### M12 lane

M12 uses:

```text
runtime/wine/lib/dxmt_m12/x86_64-windows
runtime/wine/lib/dxmt_m12/x86_64-unix
```

Global M12 launch env must include:

```text
WINEDLLPATH=.../lib/dxmt_m12/x86_64-windows
DYLD_LIBRARY_PATH includes .../lib/dxmt_m12/x86_64-unix
DYLD_FALLBACK_LIBRARY_PATH includes .../lib/dxmt_m12/x86_64-unix
WINEDLLOVERRIDES=winemetal,d3d12,dxgi,dxgi_dxmt,d3d11,d3d10core=n,b;gameoverlayrenderer,gameoverlayrenderer64=d
DXMT_WINEMETAL_UNIXLIB=winemetal.so
DXMT_CONFIG_FILE=.../runtime/wine/etc/dxmt.conf
MS_GRAPHICS_BACKEND=dxmt
WINEMSYNC=1
```

M12 deploy DLL set:

```text
d3d12.dll
d3d11.dll
dxgi.dll
dxgi_dxmt.dll
d3d10core.dll
winemetal.dll
nvapi64.dll
nvngx.dll
```

M12 Unix sidecars:

```text
winemetal.so
libc++.1.dylib
libc++abi.1.dylib
libunwind.1.dylib
```

## Implementation roadmap

### Phase 0 — Freeze artifact truth

Define artifact truth before further implementation:

- `metalsharp-runtime.tar.zst` contains Wine, host ABI, backend, and MetalSharp hook DLL.
- `metalsharp-graphics-dll.tar.zst` contains both `Graphics/dll/dxmt` and `Graphics/dll/dxmt-m12`.
- `metalsharp-d3d12-developer-sdk.tar.zst` is regenerated from the repaired runtime + repaired graphics bundles.
- `metalsharp-bundle-manifest.tsv` is regenerated after runtime, graphics, and SDK changes.

Acceptance:

- Bundle verifier confirms runtime backend and host ABI are present.
- Graphics verifier confirms both M11 and M12 runtime surfaces are present.
- SDK verifier confirms M11 and M12 runtime material is present.

### Phase 1 — Split runtime readiness APIs

Replace one ambiguous DXMT readiness model with explicit lane readiness:

```rust
ensure_dxmt_runtime_ready(home)       // M9/M10/M11 lane only
ensure_dxmt_m12_runtime_ready(home)   // M12 lane only
ensure_graphics_runtimes_ready(home)  // full setup/new install/update
```

Add lane status fields:

```json
{
  "dxmt": {
    "current": true,
    "filesReady": true,
    "path": ".../runtime/wine/lib/dxmt"
  },
  "dxmt_m12": {
    "current": true,
    "filesReady": true,
    "path": ".../runtime/wine/lib/dxmt_m12"
  }
}
```

Acceptance:

- M11 readiness does not require `dxmt_m12`.
- M12 readiness does require `dxmt_m12`.
- Full setup readiness requires both.

### Phase 2 — Setup wizard and dependency model

Update setup dependencies to list both graphics lanes:

- `dxmt_runtime` — M9/M10/M11 runtime under `runtime/wine/lib/dxmt`.
- `dxmt_m12_runtime` — M12 runtime under `runtime/wine/lib/dxmt_m12`.

Setup/install-all should prepare both for new installs and migrations.

Acceptance:

- New install gets both lanes.
- Setup UI reports which lane is missing/stale.
- Repair commands exist for both lanes.

### Phase 3 — Bottle save / route switch contract

Create one authoritative save-and-prepare operation for Steam bottle pipeline changes:

```text
selected pipeline
→ resolve runtime profile
→ ensure selected shared runtime lane
→ run PR #230 prepare path for selected pipeline
→ verify deployed DLLs and launch env
→ update bottle manifest preferred_pipeline/runtime_profile
→ do not write compatdata
```

For M11:

- Prepare `dxmt` lane.
- Verify no `dxmt_m12` path appears.
- Verify M11 DLLs from `runtime/wine/lib/dxmt`.

For M12:

- Prepare `dxmt_m12` lane.
- Verify full 8-DLL deploy set.
- Verify M12 Unix sidecars.
- Verify `WINEDLLOVERRIDES` includes `winemetal,d3d12,dxgi,dxgi_dxmt,d3d11,d3d10core=n,b`.
- Verify `WINEDLLPATH` and DYLD paths target `dxmt_m12`.
- Stage/verify D3D12 Agility only if the game/title requires it.

Important correction:

- Do not mark `d3d12_agility` installed merely because shared M12 runtime is ready.
- Agility must be verified with the existing game-local/shared-payload inspection path.

Acceptance:

- Saving M11 after M12 restages/verifies M11 from `dxmt`.
- Saving M12 after M11 restages/verifies M12 from `dxmt_m12`.
- Bottle manifest reflects the selected route.
- No compatdata is written.

### Phase 4 — Repair endpoints and doctor reports

Standardize repair coverage through `/bottles/repair-component` and doctor/prepare endpoints.

Shared runtime repair components:

```text
dxmt_runtime
dxmt_m12_runtime
graphics_runtime
runtime_backend
host_runtime
metalsharp_hook
```

Game-local route repair components:

```text
m11_route_dlls
m12_route_dlls
d3d12_agility
gpu_vendor_stubs
```

Doctor output should include:

```json
{
  "pipeline": "m12",
  "runtimeLane": "dxmt_m12",
  "sharedRuntimeReady": true,
  "gameLocalDllsReady": true,
  "launchEnvReady": true,
  "missing": [],
  "repairActions": []
}
```

Acceptance:

- M11 repair never pulls from `dxmt_m12`.
- M12 repair always pulls from `dxmt_m12`.
- Doctor names every missing DLL/sidecar.
- Prepare and launch share the same path.

### Phase 5 — Remove compatdata

Retire compatdata fully.

Remove/replace:

```text
~/.metalsharp/compatdata
metalsharp-compatdata.json
SteamCompatdataRecord
/steam/compatdata writes
compatdata launch log paths
migration preserve/restore compatdata
```

Move launch logs to one of:

```text
~/.metalsharp/bottles/steam_{appid}/logs
```

or:

```text
~/.metalsharp/logs/steam/{appid}
```

Migration change:

- Bump `MIGRATE_SCHEMA_VERSION`.
- Do not preserve compatdata.
- Do not restore compatdata.
- Remove existing `~/.metalsharp/compatdata`.
- Migration report should state compatdata was deprecated/removed and route state now lives in bottle manifests.

Endpoint change:

- Remove `/steam/compatdata`, or make it return deprecated metadata only:

```json
{
  "ok": false,
  "deprecated": true,
  "replacement": "bottle manifest route state"
}
```

Acceptance:

- Fresh install never creates compatdata.
- Migration removes existing compatdata.
- Launch no longer writes compatdata.
- Tests no longer assert compatdata preservation.

### Phase 6 — Remove current anti-cheat/EAC implementation

Remove/disable:

```text
Offline EAC Mode setup step
install_eac_toggle
eac-toggle bundle requirement
assets/eac-toggle verifier requirement
deploy_eac_toggle
anticheat fields in configs/mtsp-rules.toml
/steam/anticheat-* endpoints
mscompatdb launch behavior if tied to current non-working implementation
EAC repair flows
```

Protected-launcher executable selection should remain only as normal exe selection/bypass logic if still needed; it should not be labeled as EAC repair.

Acceptance:

- Setup does not install EAC toggle.
- Bundle verification does not require EAC toggle.
- Rules have no `anticheat = ...` entries.
- No EAC repair path is exposed.

### Phase 7 — Global M12 launch shape and logging controls

Make the global M12 node the only default M12 launch shape.

Keep default M12 env minimal and production-safe:

```text
WINEDLLOVERRIDES
WINEDLLPATH
DYLD_LIBRARY_PATH / DYLD_FALLBACK_LIBRARY_PATH
DXMT_WINEMETAL_UNIXLIB
DXMT_CONFIG_FILE
MS_GRAPHICS_BACKEND
WINEMSYNC
cache paths except graphics logs
```

Remove default-on graphics logging:

- Do not export `DXMT_LOG_PATH` unless developer logging is enabled.
- Remove/gate hardcoded Subnautica 2 M12 trace/debug env.
- Add a developer setting/API/UI flag such as `graphicsRuntimeLogs`.

When enabled, developer logging can add:

```text
DXMT_LOG_PATH
DXMT_D3D12_TRACE
DXMT_DXGI_TRACE
DXMT_WINEMETAL_DEBUG
DXMT_DUMP_MSL
```

Acceptance:

- Normal M12 launch has no `DXMT_LOG_PATH`.
- Developer logging opt-in does add expected log env.
- Final Elden launch can prove no logs are emitted by default.

### Phase 8 — Rules cleanup for all M12 games

Audit every `pipeline = "m12"` rule in `configs/mtsp-rules.toml`.

For every M12 game:

- No stale anti-cheat field.
- No default trace/debug env.
- No custom env overriding global M12 routing unless explicitly justified.
- Diagnostics include at least:

```text
d3d12.dll
dxgi.dll
dxgi_dxmt.dll
winemetal.dll
```

Acceptance tests:

```text
all M12 rules inherit global M12 override shape
all M12 rules have no anti-cheat field
all M12 rules have no default trace/debug env
all M12 diagnostics include dxgi_dxmt + winemetal
```

### Phase 9 — Bundle/release build order

After backend changes are complete:

```bash
cd app/src-rust
cargo fmt --all -- --check
cargo clippy --all-targets -- -D warnings
cargo test --no-fail-fast
cargo build --release
codesign --force --sign - target/release/metalsharp-backend
```

Then:

1. Repair/rebuild `metalsharp-runtime.tar.zst` with current backend + host ABI.
2. Repair/rebuild `metalsharp-graphics-dll.tar.zst` with current `dxmt_m12` payload.
3. Regenerate developer SDK.
4. Regenerate bundle manifest.
5. Verify bundles and SDK.
6. Verify M11 preservation.
7. Verify M12 hashes.
8. Verify Mach-O signing.
9. Do not upload until explicitly approved.

Acceptance:

- `tools/bundles/verify-bundles.sh --bundle-dir ... --require mac` passes.
- `tools/bundles/verify-developer-sdk.sh ...` passes.
- Runtime tarball contains updated backend.
- SDK contains updated runtime and graphics payload.
- Manifest hashes/sizes match actual archives.

### Phase 10 — New install validation

Fresh install must prove:

```text
setup installs runtime bundle
setup installs host runtime
setup installs backend from runtime tarball
setup installs dxmt lane
setup installs dxmt_m12 lane
setup installs rules
setup does not install EAC toggle
setup does not create compatdata
M11 bottle save prepares dxmt
M12 bottle save prepares dxmt_m12
```

### Phase 11 — Migration validation

Update migration must prove:

```text
old runtime removed
new runtime installed
new backend installed from runtime tarball
dxmt and dxmt_m12 both current
existing bottles preserved
existing preferred pipeline preserved
compatdata removed/not restored
old EAC assets not restored
old M12 bottles re-prepare on next save/launch
migration report explains removed compatdata
```

### Phase 12 — Final approved launch proof

Only after implementation and bundle verification:

1. Restart backend from updated runtime.
2. Run `/mtsp/prepare` for Elden Ring M12.
3. Verify no default DXMT logs are enabled.
4. Launch Elden Ring with M12 after explicit approval.
5. Let it run for about 25 seconds.
6. Close cleanly.
7. Capture proof:
   - launch response,
   - env proof,
   - no-log proof,
   - staged DLL hash proof,
   - process close proof.

Only after this audit can the active goal be marked complete.

## Test matrix to add/update

### Runtime tests

- `dxmt_runtime_ready_does_not_require_dxmt_m12`
- `dxmt_m12_runtime_ready_requires_isolated_sidecars`
- `graphics_runtime_status_reports_both_lanes`
- `runtime_bundle_requires_backend_and_host_abi`

### Bottle tests

- `m11_bottle_save_prepares_legacy_dxmt_lane`
- `m12_bottle_save_prepares_isolated_dxmt_m12_lane`
- `switch_m11_to_m12_updates_profile_and_prepare_contract`
- `switch_m12_to_m11_removes_m12_lane_from_prepare_env`
- `d3d12_agility_is_game_local_not_shared_runtime_ready`

### Prepare/doctor tests

- `m12_prepare_uses_pr230_launch_shape`
- `m11_prepare_does_not_use_dxmt_m12`
- `m12_doctor_reports_missing_dxgi_dxmt`
- `m12_doctor_reports_missing_winemetal_unix_sidecars`

### Compatdata tests

- `fresh_launch_does_not_write_compatdata`
- `migration_removes_compatdata`
- `steam_compatdata_endpoint_is_deprecated_or_removed`

### Anti-cheat tests

- `setup_steps_do_not_include_eac_toggle`
- `bundle_verifier_does_not_require_eac_toggle`
- `rules_have_no_anticheat_entries`

### Logging tests

- `m12_default_env_has_no_dxmt_log_path`
- `developer_graphics_logs_enable_dxmt_log_path`
- `subnautica2_m12_trace_env_is_opt_in_only`

## Open questions before implementation

1. Should deprecated `/steam/compatdata` be removed entirely, or left as a deprecated no-op response for one release?
2. Where should new launch logs live permanently: bottle-local logs or global `logs/steam/{appid}`?
3. Should developer graphics logging be a global setting, per-game setting, or both?
4. Should EAC/anti-cheat endpoints be removed outright or return `501 Not Implemented` pending replacement?
5. Should migration immediately re-prepare saved M12 bottles, or defer prepare until next save/launch to avoid touching commercial game directories during update?
