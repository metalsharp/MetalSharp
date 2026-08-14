# MetalSharp Phased Optimization Roadmap
**Updated:** 2026-07-08


This directory documents the 9-phase optimization roadmap that hardens
MetalSharp's launch routes, bottles, VKD3D artifact verification, shader/PSO
diagnostics, descriptor binding, command replay, runtime/migration cleanup,
Mono/FNA/XNA reliability, and release gates.

Each phase landed as its own commit on the `codex/phased-optimization-roadmap`
branch with its own proof gate (`cargo fmt --check`, `cargo clippy -D
warnings`, `cargo test` all green) and kept DXMT/VKD3D launch behavior and
artifact paths untouched.

| Phase | Commit prefix | Module / surface |
|-------|---------------|------------------|
| 1 — Baseline observability | `feat(diagnostics)` | `diagnostics.rs`, launch timing, injection hashes |
| 2 — Bottle/route contract | `feat(bottles)` | `SteamRouteContract`, migration report |
| 3 — VKD3D artifact verifier | `feat(mtsp)` | `vkd3d_verify_dry_run`, pipeline dry-run |
| 4 — Shader/PSO/cache | `feat(shader_cache)` | cache doctor, `PsoDiagnosticManifest` |
| 5 — Descriptor binding | `feat(binding_contract)` | root-signature + reflection ABI |
| 6 — Command replay/barriers | `feat(command_contract)` | command-list/visibility validator |
| 7 — Runtime/migration cleanup | `feat(installer)` | artifact report, `WinebootState`, stop-targets |
| 8 — Mono/FNA/XNA | `feat(fna_profile)` | signals, receipts, profile-explain, classifier |
| 9 — Release gates/docs | `docs` | local gates, release checklist, CI notes |

## Documents

- [`PR-SUMMARY.md`](PR-SUMMARY.md) — per-phase summary, what landed, proof.
- [`local-gates.md`](local-gates.md) — the canonical local gates (Rust, TS,
  C++, SDK probes, diagnostic routes).
- [`release-checklist.md`](release-checklist.md) — pre-release verification
  items (version sync, runtime artifacts, graphics gates, route gates).
- [`ci-gating-notes.md`](ci-gating-notes.md) — what CI proves vs what must run
  locally.

## Baseline and proof

- Baseline before any work: **502 Rust tests passed, 0 failed.**
- Final: **594 Rust tests passed, 0 failed**, clippy + fmt clean.
- `validate-contracts.py` → `[PASS]` (8 contracts)
- `validate-probe-matrix.py` → `[PASS]` (18 probe groups)
