# CI Gating Notes
**Updated:** 2026-07-08


CI (`pr-ci.yml`) covers shell, Metal, Vue, Rust, Electron, DMG-workflow, and
C/C++/Obj-C jobs. The **graphics gates** (D3D12 Metal SDK probes and contract
validation) require a host Wine/Metal runtime and therefore run **locally**,
not in CI. This doc makes that requirement obvious so a reviewer does not
mistake a green CI run for graphics proof.

## What CI proves

- Rust: `cargo fmt --check`, `cargo clippy -D warnings`, `cargo test`.
- TypeScript/Electron: build + `biome check`.
- C++: native engine + tests compile.
- DMG workflow contract validation.

## What CI cannot prove (run locally)

The D3D12 Metal SDK probes, contract validation, and runtime-layout preflight
all execute a Wine/Metal probe suite that needs the MetalSharp runtime
installed on the host. Run them locally before merging any graphics/contract
change:

```bash
python3 tools/d3d12-metal-sdk/scripts/validate-contracts.py
python3 tools/d3d12-metal-sdk/scripts/validate-probe-matrix.py
python3 tools/d3d12-metal-sdk/scripts/preflight-runtime-layout.py --profile metalsharp
# and, with a host runtime available:
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --mini-only
```

## Backend contract validators (Phase 4–6)

The optimization roadmap added typed Rust validators that mirror what the SDK
probes check. These are unit-tested in `cargo test` (so CI does prove them),
and are also exposed as routes for live introspection:

- `POST /diagnostics/binding-contract/validate` — Phase 5 root-signature +
  reflection ABI validation.
- `POST /diagnostics/command-replay/validate` — Phase 6 command-list / barrier
  / resource-visibility validation.
- `GET /diagnostics/cache-doctor?appid=` — Phase 4 cache introspection.
- `GET /diagnostics/pso-manifests?appid=` — Phase 4 PSO trace manifests.

## VKD3D isolation

VKD3D's default backend (`lib/vkd3d-proton` + `lib/dxvk` + `lib/moltenvk-vkmt`)
is an isolated lane that may advance independently; the `vkd3dBackend=dxmt`
rollback (`lib/dxmt-vkd3d`) is a protected compatibility lane. M9/M10/M11 are
protected compatibility lanes that share the legacy `lib/dxmt` surface. A
graphics PR that touches VKD3D must not disturb M9/M10/M11 artifact paths; the
contract tests in `mtsp::launcher::tests` enforce this.

See `docs/architecture/vkd3d-pipeline-map.md` for the full VKD3D route definition
and the dry-run verifier.
