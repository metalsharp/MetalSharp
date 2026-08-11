# AGENTS.md

**Updated:** 2026-08-09

Guide for AI agents and contributors working on the MetalSharp repository. Keep
this file aligned with the current code, release bundles, CI workflows, and
the canonical architecture documents under `docs/`.

## What This Project Is

MetalSharp is a macOS application for running Windows Steam games and Windows
programs through Wine and Metal translation. It combines an Electron UI, a
Rust HTTP backend, C++/Objective-C++ native surfaces, MTSP per-game pipeline
routing, isolated runtime bottles, installer profiles, FNA/Mono support, and
release packaging for macOS and Linux.

The repository contains several different execution surfaces. Do not assume
that the in-tree native C++ D3D12 implementation, the Wine-launched M12
pipeline, GPTK, DXMT, and the FNA/Mono lane are interchangeable. Before
changing an existing surface, identify which launcher, runtime directory,
bundle, prefix, and diagnostic contract own it.

## Working on Existing Surfaces

Start with the smallest relevant seam and preserve the surrounding contracts:

1. Read the relevant architecture/runtime document and nearby tests first.
2. Trace the complete path before editing: renderer/API call → Rust route →
   MTSP pipeline or bottle operation → installer/runtime staging → launcher
   environment → logs/diagnostics.
3. Preserve existing route ids, saved config keys, bottle ids, migration
   behavior, and fallback routes unless the change explicitly requires a
   compatibility migration.
4. For a pipeline change, update the route definition, PE/rule selection,
   launcher environment, installer artifact requirements, dry-run/doctor
   output, and tests as one change. Do not fix only the UI label or only the
   DLL copy step.
5. Use `METALSHARP_HOME` and the helpers in
   `app/src-rust/src/platform.rs` in tests and new code. Never add absolute
   `/Users/...` paths, machine-specific paths, secrets, or generated build
   output.
6. Treat release bundles as immutable, verified inputs. Do not hand-edit a
   downloaded archive or replace a pinned graphics payload from an arbitrary
   local build. If a payload really changes, update the manifest, verifier,
   hash data, installer requirements, and release workflow together.
7. Keep pull requests focused and do not add files at the repository root
   without maintainer agreement. Update compatibility/docs for user-visible
   behavior.

The canonical preparation endpoint is `POST /mtsp/prepare`. The older
`POST /game/prepare` endpoint remains as a compatibility wrapper and should not
be used for new frontend/backend work. Use the repository's shared pre-commit
hook when possible; it deliberately fails when required tools are missing.

### Recommended Tools and Agent Skills

- Use `rg`, `find`, `git log`, `git blame`, and focused `git diff` searches to
  map an existing surface before editing it. Prefer repository scripts over
  ad-hoc replacement commands.
- Use `cargo`/rustfmt/clippy for Rust, CMake/CTest/clang-format for native
  code, `npm`/TypeScript/Biome/Prettier for the app, and Python plus
  ShellCheck for tooling and validation. Use `gh` for workflow/PR state, not
  for bypassing repository checks.
- For route/runtime changes, first read the matching architecture/runtime docs
  and `docs/optimization-roadmap/local-gates.md`; before declaring a surface
  complete, run the applicable commands in Build and Test Commands and get an
  independent diff review. If the environment provides separate discovery,
  validation, or review skills, use them for those passes. If it supports web
  research, use it only to confirm current upstream DXMT, vkd3d-proton, VKMT,
  Wine, or Apple SDK behavior; the checked-in source and pinned bundle
  manifests remain authoritative for this repository.
- For broad changes, split discovery, implementation, validation, and review
  into separate passes. Do not let a speculative refactor hide a runtime
  behavior change.

## Repository Structure

```
app/
├── src-rust/                  Rust HTTP backend and route/pipeline logic
│   └── src/
│       ├── main.rs            HTTP router and process lifecycle
│       ├── mtsp/              pipeline engine, PE/rules, recipes, launcher,
│       │                       input shims, shader caches, and contracts
│       ├── launch.rs          legacy/config launch helpers and game control
│       ├── bottles.rs         bottle profiles, doctor, redists, migration data
│       ├── installer.rs       split bundle installation and hash validation
│       ├── steam.rs           Wine Steam lifecycle, libraries, CEF, install
│       ├── setup.rs           dependency/setup state and install operations
│       ├── sharp_library.rs   imported applications and installer bottles
│       ├── gog.rs             GOG authentication, sync, install, and launch
│       ├── diagnostics.rs     artifact hashes, launch/pipeline diagnostics
│       ├── d3d12_runtime_doctor.rs
│       ├── d3dmetal_gptk.rs   GPTK/D3DMetal integration surface
│       ├── binding_contract.rs and command_contract.rs
│       ├── fna_profile.rs and mono_profile.rs
│       ├── kernel_translation/ NT/XNU compatibility research surface
│       ├── launcher_evidence.rs, metalfx.rs, migrate.rs, platform.rs, scan.rs
│       └── updater.rs         GitHub release update/migration operations
├── src/main/                  Electron main process and Rust bridge
├── src/renderer/              Vue renderer, library, setup, logs, settings
├── bundles/                   local copies of downloaded split release assets
├── native/                    locally built host runtime/native placeholders
├── updater/                   Python update runtime
├── package.json               Electron scripts and packaging metadata
└── src-rust/Cargo.toml        Rust backend manifest

src/                           in-tree native C++/Obj-C++ D3D and Metal code
include/                       public native headers
tests/                         C++/Obj-C++ tests and PE smoke tests
vendor/dxmt/                   vendored/submodule DXMT source and probes
vendor/glslang/, vendor/SPIRV-Cross/ shader/compiler dependencies
configs/                       MTSP rules, DLL maps, and proof targets
docs/                          architecture, runtime, compatibility, and gates
tools/bundles/                 asset manifest, split-bundle and SDK tooling
tools/d3d12-metal-sdk/         contracts, probes, audits, and M12 SDK scripts
tools/ci/                      CI validation, formatting, and contract checks
tools/dmg/                     bundle staging, DMG, and runtime verification
tools/diagnostics/             launch/environment probes
tools/package/, tools/release/ host/runtime and release helpers
scripts/tools/native/          native helper build and process-manager tools
```

Clone with submodules when working on native/vendor surfaces:

```bash
git clone --recurse-submodules https://github.com/metalsharp/MetalSharp.git
```

## Rust Backend and MTSP

The backend is a bin-only `tiny_http` server listening on
`127.0.0.1:9274`. Set `METALSHARP_PORT` to override the port and
`METALSHARP_HOME` to override the data root. `main.rs` owns the HTTP
dispatch, requires the per-process token in
`$METALSHARP_HOME/.backend-token` on every request, and owns the trust boundary
for registered running-game PIDs; do not make global stop endpoints accept
arbitrary system PIDs. Trusted diagnostics should read that file and send the
value as `X-MetalSharp-Token`; never put the token in source, logs, or URLs.

The Electron main process supplies a fresh per-session `METALSHARP_API_TOKEN`
to the backend and attaches it to every API request. The backend rejects
missing or invalid bearer tokens before dispatching routes; `/health` is the
only unauthenticated endpoint and returns version/readiness information only.
Browser origins are defense-in-depth and are limited to the fixed Vite origins
(`http://localhost:5173` and `http://127.0.0.1:5173`); packaged Electron UI
requests use the main-process bridge rather than direct browser CORS.

The backend's important layers are:

- `mtsp/engine.rs` defines public and internal pipeline nodes.
- `mtsp/pe.rs` and `mtsp/rules.rs` classify executable imports and apply
  `configs/mtsp-rules.toml`.
- `mtsp/recipe.rs` prepares DLLs, shims, prefixes, and route-specific assets.
- `mtsp/launcher.rs` builds the actual Wine command, environment, overrides,
  cache paths, evidence, and FNA/Mono handoff.
- `mtsp/shader_cache.rs` keeps shader/pipeline caches per route and app id.
- `bottles.rs` owns bottle manifests, runtime profiles, Windows-version modes,
  redist checks, compatibility records, and bottle doctors.
- `installer.rs` downloads/verifies/stages split bundles, preserves graphics
  surfaces during runtime replacement, reconciles M12 fallback state, and
  enforces pinned hashes.
- `diagnostics.rs`, `d3d12_runtime_doctor.rs`, `binding_contract.rs`, and
  `command_contract.rs` expose read-only or validation contracts used by local
  gates and CI-adjacent tooling.

Important backend route groups include:

- `/mtsp/*` for pipeline catalogs, launch shape, preparation, recipes, and
  doctors; use these for new routing work.
- `/game/*` for launch-auto, routing compatibility, running-game state, and
  process control.
- `/steam/*` for Steam lifecycle/library/game launch, bridge, compatdata,
  runtime doctors, and scoped stop targets.
- `/bottles/*` for profiles, route contracts, compatibility, preparation,
  component repair, Windows version, installer relaunch, and DirectX checks.
- `/sharp-library/*` for imported apps, installer-bottle apps, GOG apps,
  covers, launch args, engine selection, and app doctors.
- `/setup/*`, `/update/*`, `/config`, and `/logs/*` for setup, migration,
  updates, configuration, structured logs, streams, and crash reports.
- `/diagnostics/*` for launch/pipeline dry-runs, artifact reports, cache/PSO
  data, FNA explanations, binding/command validation, and Wine boot state.

When adding or changing a route, update the corresponding renderer API types,
route tests, diagnostics, and this guide only when the public contract changes.
Keep compatibility aliases documented rather than silently removing them.

## Graphics Pipelines and the Rust M12 Update

The current M12 implementation changed after v0.59.1. **M12 now defaults to
vkd3d-proton**, not DXMT:

```text
M12 default:  D3D12 → vkd3d-proton → Vulkan → VKMT MoltenVK → Metal
M12 rollback intent: D3D12 → DXMT/winemetal → Metal (m12Backend=dxmt)
M11:          D3D11 → DXMT → Metal
M10:          D3D10 → DXMT → Metal
M9:           D3D9  → the current Metal/DXMT-family handoff
```

Read [`docs/architecture/m12-pipeline-map.md`](docs/architecture/m12-pipeline-map.md)
before touching M12. The default M12 route is x86_64-only and stages:

The architecture map describes the intended DXMT rollback, but the current
Rust source/tests expose only the vkd3d-proton M12 node. Treat the source and
tests as authoritative until that document and the implementation are
reconciled; do not present the rollback as a working route today.

- `lib/vkd3d-proton/x86_64-windows/d3d12.dll` and `d3d12core.dll`;
- `lib/dxvk/x86_64-windows/dxgi.dll` and the accompanying D3D11 surface;
- `lib/moltenvk-vkmt/libMoltenVK.dylib` and `MoltenVK_icd.json`; and
- optional `nvapi64.dll`/`nvngx.dll` stubs from the DXMT M12 lane when a game
  or bottle component requires them; they are not part of the default M12
  node's core DLL deploy list.

The setting `m12Backend=vkd3d-proton` is the default. `m12Backend=dxmt` is a
recognized configuration value and installer/runtime rollback intent, but the
current `PipelineNode::M12` implementation and its tests expose only the
vkd3d-proton node. Do not claim that the DXMT rollback is functional without
first implementing and testing a distinct node in `mtsp/engine.rs` and
`mtsp/launcher.rs`. The rollback surface is still staged under
`lib/dxmt_m12` for that work. The release archive calls it
`Graphics/dll/dxmt-m12` (hyphen); the installed runtime directory uses
`lib/dxmt_m12` (underscore). Do not conflate it with the known-good
M9/M10/M11 `lib/dxmt` surface.

The vkd3d-proton launch path pins `VK_ICD_FILENAMES` to the VKMT ICD and uses
`VKD3D_SHADER_CACHE_PATH`/`DXVK_STATE_CACHE_PATH`. `DXMT_CONFIG_FILE` and
`DXMT_WINEMETAL_UNIXLIB` are used by the other DXMT-backed nodes; do not assume
they apply to an M12 `m12Backend=dxmt` request until the distinct rollback node
is implemented. M12 route aliases such as `m12`, `dx12`, and `d3d12` select a
pipeline; they are not universal game command-line arguments. Keep Wine Steam
alive as the client when a Steam route requires it, and launch the game with
the selected bottle, prefix, Steam identity variables, and route environment.

The M12 installer validates pinned vkd3d-proton and VKMT MoltenVK hashes. If a
graphics bundle changes, update the source/manifest/hash contract together;
do not set `METALSHARP_REPAIR_M12=1` casually. The default bundle workflow
preserves the canonical M12 payload specifically to prevent local artifacts
from silently replacing the release lane.

## Runtime, Prefix, Cache, and Bundle Paths

`~/.metalsharp` is the default root; use `METALSHARP_HOME` in tests. Important
paths are:

| Path | Purpose |
|---|---|
| `~/.metalsharp/runtime/wine/` | bundled Wine runtime and backend-facing Wine files |
| `~/.metalsharp/runtime/host/` | host runtime ABI manifest/header/library |
| `~/.metalsharp/runtime/metalsharp-backend` | staged Rust backend executable |
| `~/.metalsharp/runtime/wine/lib/dxmt/` | M9/M10/M11 DXMT surface |
| `~/.metalsharp/runtime/wine/lib/dxmt_m12/` | M12 DXMT rollback surface |
| `~/.metalsharp/runtime/wine/lib/vkd3d-proton/` | default M12 D3D12 DLLs |
| `~/.metalsharp/runtime/wine/lib/dxvk/` | DXVK DLL lanes used by M12/fallbacks |
| `~/.metalsharp/runtime/wine/lib/moltenvk-vkmt/` | patched MoltenVK and VKMT ICD |
| `~/.metalsharp/runtime/wine/lib/wine/x86_64-unix/` | Wine unix libraries and shared sidecars |
| `~/.metalsharp/runtime/wine/etc/dxmt.conf` | DXMT configuration |
| `~/.metalsharp/runtime/bundle-state/` | installed split-bundle SHA-256 markers |
| `~/.metalsharp/runtime/redist/` | VC++, Agility, FNA, and other local redistributables |
| `~/.metalsharp/runtime/mono-*`, `fna*`, `fnalibs/`, `xna/`, `unity-mono/` | FNA/Mono and game support assets |
| `~/.metalsharp/bottles/<id>/` | bottle metadata, prefix, assets, and logs |
| `~/.metalsharp/bottles/<id>/prefix/` | bottle-specific Wine prefix |
| `~/.metalsharp/sharp-prefix/` | shared Sharp Library prefix |
| `~/.metalsharp/prefix-steam/` | live Wine Steam prefix |
| `~/.metalsharp/games/<appid>/` | local game copies/staging |
| `~/.metalsharp/shader-cache/<route>/<appid>/` | per-app shader/pipeline caches |
| `~/.metalsharp/logs/` | backend, launch, crash, and migration logs |
| `~/.metalsharp/cache/bundles/` | downloaded release archives |

Never assume a game directory's copied DLLs are authoritative without checking
the bottle manifest and route dry-run. Installer bottles must retain their
`bottle_id` when apps are imported so Sharp Library launches them from the
correct prefix.

## Release Bundles and Where to Find Them

The canonical prebuilt assets are published in the `bundles` release of the
`aaf2tbz/metalsharp` GitHub repository, not necessarily committed to this
checkout. A development checkout may have local copies in `app/bundles/`.
`tools/dmg/create-bundles.sh` downloads and verifies them; the Rust installer
also searches packaged resources, `app/bundles/`, and
`~/.metalsharp/cache/bundles/`.

The split assets and roots are defined in
[`tools/bundles/asset-manifest.tsv`](tools/bundles/asset-manifest.tsv):

| Archive | Root/content |
|---|---|
| `metalsharp-electron.tar.zst` | Electron application payload |
| `metalsharp-graphics-dll.tar.zst` | DXMT, vkd3d-proton, DXVK, and VKMT MoltenVK lanes |
| `metalsharp-runtime.tar.zst` | Wine, host runtime ABI, and backend |
| `metalsharp-assets.tar.zst` | Mono, GPTK/DXVK support, FNA/XNA/Unity/SDL assets |
| `fnalibs.tar.zst` | FNA3D, FAudio, SDL2, theorafile, and FMOD payloads |
| `metalsharp-scripts-tools.tar.zst` | updater scripts, configs, native tools, CEF helpers |
| `metalsharp-steam.tar.zst` | Steam installer and CEF wrapper assets |
| `metalsharp-d3d12-developer-sdk.tar.zst` | D3D12 contracts, probes, docs, staged runtime |

Download and verify release archives without rewriting them:

```bash
METALSHARP_REPAIR_BUNDLES=0 METALSHARP_SKIP_DEVELOPER_SDK_BUNDLE=1 \
  tools/dmg/create-bundles.sh
tools/bundles/verify-bundles.sh --bundle-dir app/bundles \
  metalsharp-electron.tar.zst metalsharp-graphics-dll.tar.zst \
  metalsharp-runtime.tar.zst metalsharp-assets.tar.zst fnalibs.tar.zst \
  metalsharp-scripts-tools.tar.zst metalsharp-steam.tar.zst
```

The developer SDK archive is optional for normal app work; verify it
separately when working on `tools/d3d12-metal-sdk/` or release publication.

The default `create-bundles.sh` mode repairs/stages local runtime outputs and
requires the built backend, host runtime, and native runtime libraries. Use
that mode only when intentionally rebuilding release inputs; use
`METALSHARP_REPAIR_M12=1` only when deliberately refreshing the pinned DXMT
rollback payload.

For release/developer SDK work, also use
`tools/bundles/create-developer-sdk.py` and
`tools/bundles/verify-developer-sdk.sh`. Do not use the old
`metalsharp_bundle.tar.zst`/`metalsharp_bundle2.tar.zst` names as current
release inputs; those are legacy source names in historical tooling only.

## Build and Test Commands

Run only the relevant gates for the surface changed, but do not claim a check
was run when it was skipped. Documentation-only changes should at minimum run
`git diff --check`, link review, and the doc freshness check.

### Rust backend (required for Rust changes)

```bash
cd app/src-rust
cargo fmt --all -- --check
cargo clippy --all-targets -- -D warnings
cargo test                 # bin-only crate; do not use cargo test --lib
cargo build --release
cd ../..
```

### Electron/TypeScript (required for app/UI changes)

```bash
cd app
npm ci
npx tsc --noEmit
npm run build
npx @biomejs/biome ci src/renderer
npx @biomejs/biome ci src/main src/shared
npx prettier --check 'src/renderer/**/*.{ts,js,html,css,json}' \
  'src/main/**/*.{ts,js,json}' 'src/shared/**/*.{ts,js,json}'
cd ..
```

### Native C++/Obj-C++ (required for native changes)

```bash
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build-native --parallel "$(sysctl -n hw.ncpu)"
ctest --test-dir build-native --output-on-failure
tools/ci/check-clang-format.sh
```

The CI smoke expression omits a few host-dependent graphics tests. Use the
full local suite when the host toolchain supports it.

### D3D12/M12 contracts (required for graphics/contract changes)

These checks are local gates because they need the appropriate Wine/Metal
runtime and are not fully reproducible in CI. The generic probe script does
not infer a temporary `METALSHARP_HOME`; pass its Wine, prefix, and runtime
paths explicitly when testing an isolated installation:

```bash
python3 tools/d3d12-metal-sdk/scripts/validate-contracts.py
python3 tools/d3d12-metal-sdk/scripts/validate-probe-matrix.py
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --mini-only \
  --wine "$HOME/.metalsharp/runtime/wine/bin/wine" \
  --prefix "$HOME/.metalsharp/tmp/d3d12-probe-prefix" \
  --dxmt-runtime "$HOME/.metalsharp/runtime/wine/lib/dxmt_m12"
```

For a targeted change, use the matching `--graphics-pso-only`,
`--compute-pso-only`, `--descriptors-only`, `--reflection-abi-only`,
`--command-replay-only`, `--barriers-render-pass-only`, or
`--resource-views-formats-only` profile. Use
`tools/d3d12-metal-sdk/scripts/compare-contract.py` and
`preflight-runtime-layout.py` when validating a staged runtime. The
`tools/ci/m12-check.sh` script exercises the DXMT M12 contract/rollback lane;
default vkd3d-proton M12 work should additionally use the M12 dry-run and
runtime-doctor endpoints below.

### Shell, rules, bundles, and packaging

```bash
tools/ci/shellcheck.sh
python3 tools/ci/validate-rules-toml.py
python3 tools/ci/check-doc-freshness.py
python3 tools/ci/verify-dmg-workflow.py
python3 tools/ci/test-verify-bundle-sha256.py
```

Run rules validation when `configs/mtsp-rules.toml` or DLL maps change; run
DMG workflow validation when release/bundle tooling changes; run the bundle
checksum verifier regression tests when `tools/ci/m12-bundle-hashes.tsv`,
`tools/ci/verify-bundle-sha256.sh`, or `tools/ci/m12-check.sh` change. Do not
build a DMG merely to validate a documentation change.

## Backend Diagnostic Gates

With a backend running on `127.0.0.1:9274`, these are the preferred diagnostic
checks before and after route/runtime work. Dry-run/artifact endpoints are
read-only; doctor and preparation endpoints may stage files, run probes, or
write logs, so use an isolated `METALSHARP_HOME`/prefix when appropriate:

Direct diagnostic requests must include `Authorization: Bearer
$METALSHARP_API_TOKEN`; the Electron bridge adds this header automatically.
Only `GET /health` is intentionally public for updater/readiness checks.

| Endpoint | Purpose |
|---|---|
| `GET /diagnostics/launch?appid=&pipeline=` | resolved route, prefix, runtime, hashes, cache dirs |
| `GET /diagnostics/launch/timing?appid=` | latest persisted launch timing |
| `GET /bottles/route-contracts` | declarative Steam route contracts |
| `GET /update/migrate/report` | migration preserve/skip report |
| `GET /diagnostics/m12/dry-run?appid=` | current M12 artifact/env resolution without launching; confirm backend mode/output |
| `GET /diagnostics/pipeline/dry-run?appid=&pipeline=` | compare route dry-runs |
| `GET /diagnostics/cache-doctor?appid=` | shader/pipeline cache counts and staleness |
| `GET /diagnostics/pso-manifests?appid=&pipeline=&limit=` | recent PSO manifests |
| `POST /diagnostics/binding-contract/validate` | root-signature/reflection ABI checks |
| `POST /diagnostics/command-replay/validate` | command-list/barrier/visibility checks |
| `GET /diagnostics/runtime-artifacts` | artifact presence and SHA-256 report |
| `GET /diagnostics/wineboot-state?appid=&verifying=true` | prefix update vs verification state |
| `POST /steam/runtime-doctor` | bottle/runtime readiness for a Steam game |
| `POST /steam/d3d12-runtime-doctor` | DXMT-oriented D3D12 runtime/SDK doctor; not proof of default VKMT lane |
| `GET /diagnostics/fna/signals`, `/explain`, `/classify` | FNA/XNA profile evidence |

For default vkd3d-proton M12 work, prefer the M12 dry-run and runtime-artifact
report and inspect their reported backend, DLL sources, hashes, and env. Treat
the D3D12 runtime doctor as DXMT-oriented until it becomes backend-aware.

For integration sanity, verify JSON from `/steam/status`, games from
`/steam/library`, and bottle metadata from `/bottles` and
`/bottles/redist-sources`. The `/game/launch-auto` and `/steam/launch-game`
checks are opt-in stateful tests: they can start processes, modify prefixes or
bottles, and write logs. Run them only with an isolated
`METALSHARP_HOME`/prefix and a test app id. Never publish unsanitized logs,
account credentials, tokens, or license keys.

## CI and Contributor Workflow

The main validation workflows are:

- `pr-ci.yml`: shell/rules/docs checks, Metal and native smoke, Rust, renderer
  and main-process TypeScript checks, bundle/D3D12 contract checks, and DMG
  workflow contract validation.
- `ci.yml`: main-push smoke coverage and the same lightweight release-contract
  validation; it does not publish release artifacts.
- `release.yml`: tag-driven split-bundle verification, developer SDK and DMG
  packaging, and release upload. Do not infer a Linux release workflow from
  the presence of generic packaging documentation; verify the current
  workflow before changing release behavior.
- `pr-readiness-check.yml`: validates the PR readiness checklist.

Install the shared hook once per clone if desired:

```bash
ln -s ../../.github/hooks/pre-commit .git/hooks/pre-commit
chmod +x .git/hooks/pre-commit
```

The hook runs hard gates for staged Rust, native, TypeScript, and rules files;
ShellCheck is optional locally but runs in CI, and doc freshness is
warn-only. See [`.github/hooks/README.md`](.github/hooks/README.md) and
`.github/CONTRIBUTING.md` for the authoritative contributor policy.

## Version Bumps

Update all five version surfaces together:

| File | Field |
|---|---|
| `app/package.json` | `version` |
| `app/package-lock.json` | root/package-lock `version` |
| `app/src-rust/Cargo.toml` | package `version` |
| `app/src-rust/Cargo.lock` | `metalsharp-backend` package `version` |
| `CMakeLists.txt` | `project(metalsharp VERSION ...)` |

The release workflow reads the version from a `v*` tag, while the Rust backend
uses `CARGO_PKG_VERSION`. Keep the files and tag synchronized; do not perform
a version bump as part of an unrelated documentation or runtime fix.

Use topic branches such as `docs/...`, `fix/...`, `feat/...`, or
`test/...`. Keep commits small and imperative. PRs must include the actual
checks run, intentional skips, compatibility/game notes when applicable, and
risk and rollback details from
[`.github/PULL_REQUEST_TEMPLATE.md`](.github/PULL_REQUEST_TEMPLATE.md).

## Common Pitfalls

- M12 is vkd3d-proton-first now. Treat DXMT as the recognized but currently
  unimplemented `m12Backend=dxmt` rollback intent, not as the default M12
  runtime.
- The release archive uses `dxmt-m12`, while the installed runtime uses
  `dxmt_m12`; `dxmt` is the separate M9/M10/M11 surface.
- M12's VKMT `MoltenVK_icd.json` must resolve the pinned
  `lib/moltenvk-vkmt/libMoltenVK.dylib`; do not silently fall back when
  validating a production M12 bundle.
- DXVK i386 DLLs are under `lib/dxvk/i386-windows/`, not
  `lib/wine/i386-windows/`; the latter contains Wine builtins.
- Shader caches are per route and app id (`shader-cache/<route>/<appid>`),
  not per executable name.
- Celeste (`504230`) and Terraria (`105600`) use the Mono/FNA lane and its
  native Mono/XNA/FNA, Steamworks, audio, and host shims.
- Steam game bottles are launch-authoritative, but Steam remains the live
  client for env-dependent Steam routes.
- Installer-bottle apps must retain `bottle_id` when imported into Sharp
  Library.
- Do not edit Wine plist state before the owning daemon/setup path creates it.
- `winemetal.so` is a unix bridge; do not invent an i386-unix copy for the
  x86_64-only M12 rollback. M10/M11 32-bit surfaces have their own i386 lane.
- `CMakeLists.txt`, `app/src-rust/Cargo.toml`, `app/src-rust/Cargo.lock`,
  `app/package.json`, and `app/package-lock.json` participate in synchronized
  version bumps; release tags must match the intended version.
- Linux Docker DEB builds can leave `dist/` root-owned; the release tarball
  script repairs ownership before writing package output.
- Steam auto-updates can overwrite the `steamwebhelper` wrapper; the deploy
  helper is expected to restore it.
