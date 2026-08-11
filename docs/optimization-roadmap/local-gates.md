# Local Gates
**Updated:** 2026-07-08


The canonical local gates a PR must pass before merge. CI runs a subset of
these; the graphics gates require a host Wine/Metal runtime and therefore run
locally, not in CI.

## Rust backend (always required)

```bash
cd app/src-rust
cargo fmt --all -- --check
cargo clippy --all-targets -- -D warnings
cargo test
cargo build --release
```

All four must pass. CI (`pr-ci.yml` → "Rust CI") enforces fmt, clippy, and
test.

## TypeScript / Electron (required for app/UI changes)

```bash
cd app
npm install
npm run build
npx biome check src/
npm test -- --runInBand
```

CI (`pr-ci.yml` → "Vue CI" / "Electron CI") enforces build and biome.

## C++ native engine + tests (required for C++ changes)

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build . --parallel $(sysctl -n hw.ncpu)
ctest --output-on-failure
```

CI (`pr-ci.yml` → "C/C++/Obj-C CI") enforces the build. Tests run locally.

## D3D12 Metal SDK contracts (required for graphics/contract changes)

These are the local graphics gates CI cannot run (they need a host
Wine/Metal runtime). Run them before any PR that touches M12, descriptors,
root signatures, command replay, barriers, or resource views.

```bash
python3 tools/d3d12-metal-sdk/scripts/validate-contracts.py
python3 tools/d3d12-metal-sdk/scripts/validate-probe-matrix.py
```

Both must print `[PASS]`. When a host Wine/Metal runtime is available, also
run the relevant probe suites:

```bash
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --mini-only
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --graphics-pso-only
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --compute-pso-only
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --descriptors-only
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --reflection-abi-only
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --command-replay-only
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --barriers-render-pass-only
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --resource-views-formats-only
python3 tools/d3d12-metal-sdk/scripts/compare-contract.py --profile metalsharp
python3 tools/d3d12-metal-sdk/scripts/preflight-runtime-layout.py --profile metalsharp
```

## Backend diagnostic routes (Phase 1–8)

These read-only diagnostic routes are available from a running backend
(`127.0.0.1:9274`) and are the local gates for the optimization roadmap. A
direct request must include `Authorization: Bearer $METALSHARP_API_TOKEN`;
the Electron bridge attaches the per-session token automatically. Only
`GET /health` is public.

| Route | Phase | What it proves |
|-------|-------|----------------|
| `GET /diagnostics/launch?appid=&pipeline=` | 1 | resolved pipeline, runtime profile, wine path, prefix, artifact hashes, cache dirs |
| `GET /diagnostics/launch/timing?appid=` | 1 | latest persisted launch timing |
| `GET /bottles/route-contracts` | 2 | declarative Steam route contract table |
| `GET /update/migrate/report` | 2 | latest migration preserve/skip report |
| `GET /diagnostics/m12/dry-run?appid=` | 3 | M12 artifact + env verification (no launch) |
| `GET /diagnostics/pipeline/dry-run?appid=&pipeline=` | 3 | generic pipeline dry-run |
| `GET /diagnostics/cache-doctor?appid=` | 4 | shader/pipeline cache entry counts + staleness |
| `GET /diagnostics/pso-manifests?appid=&pipeline=&limit=` | 4 | recent PSO trace manifests |
| `POST /diagnostics/binding-contract/validate` | 5 | root-signature + reflection ABI validation |
| `POST /diagnostics/command-replay/validate` | 6 | command-list/barrier/visibility validation |
| `GET /diagnostics/runtime-artifacts` | 7 | per-artifact presence + sha256 |
| `GET /diagnostics/wineboot-state?appid=&verifying=true` | 7 | prefix updating vs MetalSharp verifying |
| `GET /steam/stop-targets` | 7 | scoped stop-Wine-Steam target list |
| `GET /diagnostics/fna/signals?gameDir=` | 8 | FNA/XNA flavor + dependency signals |
| `GET /diagnostics/fna/explain?appid=&gameDir=` | 8 | profile selection explanation |
| `GET /diagnostics/fna/classify?appid=&gameDir=` | 8 | conservative unproven-game classification |
