# Release Checklist
**Updated:** 2026-07-08


Before tagging a release (`vX.Y.Z`), verify every item below. This checklist
exists so a release cannot accidentally imply M12 proof applies to M11, or
vice versa.

## Version synchronization

All five files must carry the same version before the tag is pushed:

| File | Field |
|------|-------|
| `app/package.json` | `"version": "X.Y.Z"` |
| `app/package-lock.json` | root/package lock `"version": "X.Y.Z"` |
| `app/src-rust/Cargo.toml` | `version = "X.Y.Z"` |
| `app/src-rust/Cargo.lock` | `metalsharp-backend` package `version = "X.Y.Z"` |
| `CMakeLists.txt` | `project(metalsharp VERSION X.Y.Z ...)` |

Keep version bumps **separate** from graphics changes unless the PR is
explicitly a release PR.

## Runtime artifacts

- [ ] Runtime bundle hash recorded and matches the shipped bundle.
- [ ] Developer SDK hash recorded.
- [ ] `GET /diagnostics/runtime-artifacts` reports `ok: true` (every required
      M11 `lib/dxmt`, M12 `lib/vkd3d-proton` + `lib/dxvk` + `lib/moltenvk-vkmt`
      (default backend) or `lib/dxmt-m12` (rollback) file present with a sha256).
- [ ] M12 default-backend sidecars present: `lib/moltenvk-vkmt/libMoltenVK.dylib`
      + `MoltenVK_icd.json`, the `lib/vkd3d-proton/x86_64-windows`
      `d3d12.dll`/`d3d12core.dll` (win64) + DXVK `dxgi.dll` PE set. (vkd3d-proton
      ships Windows DLLs only — there is NO vkd3d-proton unix sidecar.)
- [ ] M12 rollback sidecars present (only when `m12Backend=dxmt` is kept in the
      bundle): `winemetal.so`, `libc++.1.dylib`, `libc++abi.1.dylib`,
      `libunwind.1.dylib` under `lib/dxmt-m12/x86_64-unix/`.
- [ ] Legacy DXMT surface present: `lib/dxmt/x86_64-unix/winemetal.so` and the
      `DXMT_REQUIRED_PE` set under `lib/dxmt/x86_64-windows/`.

## Graphics gates (local; CI cannot run these)

- [ ] `python3 tools/d3d12-metal-sdk/scripts/validate-contracts.py` → `[PASS]`
- [ ] `python3 tools/d3d12-metal-sdk/scripts/validate-probe-matrix.py` → `[PASS]`
- [ ] `python3 tools/d3d12-metal-sdk/scripts/preflight-runtime-layout.py --profile metalsharp` clean
- [ ] `tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --mini-only` passes
      (`rtv_clear`, `texture_sample`, `swapchain_present`)

## Bottle / route gates

- [ ] `cargo test --manifest-path app/src-rust/Cargo.toml bottles::tests` passes
- [ ] `cargo test --manifest-path app/src-rust/Cargo.toml mtsp` passes
- [ ] `GET /bottles/route-contracts` reports every protected lane (M9, M10,
      M11, M12, FnaArm64, WineBare, D3DMetal)
- [ ] `GET /update/migrate/report` shows the expected preserved/skipped
      categories after a migration smoke

## Smoke (optional, after the gates above pass)

- [ ] Bottle migration smoke: migrate a seeded prefix and confirm the
      preserve/skip report lists the expected categories.
- [ ] Steam route smoke: a known M11 title (e.g. Portal 2) launches and
      `GET /diagnostics/launch?appid=` reports the resolved pipeline.

## Strict SDK gate

- [ ] `docs/architecture/m12-pipeline-map.md` is current and names the exact
      supported route (D3D12 → vkd3d-proton → Vulkan → VKMT MoltenVK → Metal,
      with DXMT rollback via `m12Backend=dxmt`).
- [ ] No doc claims "D3D12 works" without naming the exact route, probes,
      feature level, and remaining gaps.
- [ ] M9/M10/M11 docs do not inherit M12 proof claims.
