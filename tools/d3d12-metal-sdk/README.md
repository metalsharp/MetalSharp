# D3D12 Metal SDK

Repo-owned SDK for D3D12 → Metal work through Wine-compatible runtimes. The probes are ordinary Windows executables, so they run under MetalSharp, standalone Wine prefixes, or DXMT development prefixes.

Every D3D12 claim should be backed by a contract (`contracts/`), a probe (`probes/`), a repeatable script (`scripts/`), a baseline or result (`baselines/`, `results/`), or an explicit unsupported/risky-stub ledger entry. No game launch is needed for any of it.

## macOS 14 deployment target

Every shipped runtime Mach-O must be built with a minimum deployment target of macOS 14.0. Rewriting Mach-O metadata after compilation doesn't count. The release audit also rejects an undefined `pipe2` in the Wine Unix runtime (issue #615):

```bash
python3 tools/ci/audit-macos-deployment-targets.py \
  app/bundles/runtime app/bundles/Graphics \
  --max 14.0 --forbid-undefined-symbol pipe2
```

GPTK's `libdxccontainer.dylib` is replaced by the ABI-compatible shim in `src/dxccontainer-macos14.cpp`. Build it against a DXC `libdxcompiler.dylib` that was itself built with `MACOSX_DEPLOYMENT_TARGET=14.0`:

```bash
DXC_SOURCE_DIR=/path/to/DirectXShaderCompiler \
DXC_BUILD_DIR=/path/to/DirectXShaderCompiler/build-macos14-x86 \
tools/d3d12-metal-sdk/scripts/build-dxccontainer-macos14.sh
```

## Running probes

```bash
# Full required matrix (includes the Winemetal ABI gate and DXIL semantic corpus)
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp

# Fast one-behavior-at-a-time mini apps; results in results/probe-mini-*-metalsharp.json
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp --mini-only

# Against any Wine/DXMT runtime
tools/d3d12-metal-sdk/scripts/run-probes.sh \
  --wine /path/to/wine --prefix "$HOME/wine-d3d12-test" --dxmt-runtime /path/to/dxmt-runtime
```

A `--dxmt-runtime` directory holds `x86_64-windows/{d3d12,dxgi,dxgi_dxmt,d3d11,d3d10core,winemetal}.dll` and `x86_64-unix/winemetal.so`.

To run a single probe group, use one of these flags: `--semantic-only`, `--agility-only`, `--caps-only`, `--dxgi-only`, `--swapchain-only`, `--graphics-pso-only`, `--compute-pso-only`, `--command-replay-only`, `--barriers-render-pass-only`, `--resource-views-formats-only`, `--winemetal-abi-only`. `--render-headless` adds the optional headless render check, which is diagnostic and not a gate.

PRs that touch `vendor/dxmt/src/{d3d12,airconv,winemetal}` or this directory must keep `--mini-only` green locally. CI validates the contracts and the probe matrix.

`build-probes.sh` stages the Agility SDK 1.619.3 payload into `out/bin/D3D12/`. Override it with `AGILITY_BIN=/path/to/agility/build/native/bin/x64`.

## Strict gate

The strict gate is the merge authority for SDK changes:

```bash
tools/d3d12-metal-sdk/scripts/prepare-dxmt-x86-llvm15.sh
python3 tools/d3d12-metal-sdk/scripts/stage-dxmt-runtime.py --profile metalsharp
python3 tools/d3d12-metal-sdk/scripts/validate-contracts.py
python3 tools/d3d12-metal-sdk/scripts/preflight-runtime-layout.py --profile metalsharp
tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp
python3 tools/d3d12-metal-sdk/scripts/compare-contract.py --profile metalsharp
python3 tools/d3d12-metal-sdk/scripts/validate-probe-matrix.py
```

`compare-contract.py` must report `pass: true`, `issues: 0`, and all required probes passing. A `compiler_primary_cache_miss` during warmup passes is expected. It is a failure only in the final pass, or when `dxil_to_msl_proven` is false. `dxil_semantics_proven` is supporting evidence and never substitutes for `dxil_to_msl_proven`.

Reported shader posture: Shader Model 6.5. SM 6.6 and WaveOps are not reported until `probe-sm66-capabilities` and `probe-wave-ops` prove them. `mesh_object_shader_pso` is a tracked gap until a real mesh/object pipeline probe exists.

Use `stage-dxmt-runtime.py` to place Winemetal. Never hand-copy `winemetal.dll`/`winemetal.so` into `system32`, `syswow64`, or `runtime/wine/lib/wine`. Steam/global copies must keep legacy exports such as `WMTSetMetalShaderCachePath`. Rebuilt DXMT copies must also expose the full bridge contract in `contracts/winemetal-bridge-contract.json`. To rebuild the x86_64 Winemetal Unix bridge on Apple Silicon without linking against arm64 Homebrew LLVM, set `METALSHARP_X86_LLVM_ROOT` for `prepare-dxmt-x86-llvm15.sh`.

## Game-specific diagnostics

None of these launch Steam or the game unless noted:

```bash
# Layout check, D3D12 probes on game-local DLLs, and shader corpus replay
tools/d3d12-metal-sdk/scripts/preflight-before-game.sh --profile subnautica2 --game-dir /path/to/Binaries/Win64
python3 tools/d3d12-metal-sdk/scripts/preflight-runtime-layout.py --profile subnautica2 --game-dir /path/to/Binaries/Win64
python3 tools/d3d12-metal-sdk/scripts/check-winemetal-abi.py --profile subnautica2 --game-dir /path/to/Binaries/Win64

# Offline shader replay and Metal PSO creation (prefers DXMT-captured pso-*.json manifests)
python3 tools/d3d12-metal-sdk/scripts/replay-shader-corpus.py --profile subnautica2 --corpus /path/to/shader-cache/m12/<appid>
python3 tools/d3d12-metal-sdk/scripts/offline-pso-factory.py --profile subnautica2 --corpus /path/to/shader-cache/m12/<appid>
python3 tools/d3d12-metal-sdk/scripts/offline-pso-factory.py --profile subnautica2 --manifest /path/to/pso-manifest.json

# Independent HLSL → SPIR-V → MSL oracle for typing questions
python3 tools/d3d12-metal-sdk/scripts/shadercross-oracle.py \
  --hlsl tools/d3d12-metal-sdk/probes/shadercross_oracle/stage_io_types.hlsl --entry VSMain --profile vs_6_6

# Bounded capture: launches through the backend for a fixed window, then kills the game
tools/d3d12-metal-sdk/scripts/capture-game-shader-corpus.sh --profile subnautica2 --seconds 20
```

Results are written under `results/`.

## Contracts

```bash
python3 tools/d3d12-metal-sdk/scripts/generate-contracts.py   # regenerate from source maps
python3 tools/d3d12-metal-sdk/scripts/validate-contracts.py
python3 tools/d3d12-metal-sdk/scripts/validate-probe-matrix.py
```

## Developer package

`metalsharp-d3d12-developer-sdk.tar.zst` bundles this directory with a staged runtime. The runtime is generated from release bundles and is never committed:

```text
developer-sdk/d3d12/
  README.md  contracts/  probes/  scripts/
  runtime/
    wine/  dxmt/{x86_64-windows,x86_64-unix}/  host/  metalsharp-backend  manifest.json
```

```bash
tar --use-compress-program=unzstd -xf metalsharp-d3d12-developer-sdk.tar.zst
cd developer-sdk/d3d12
source scripts/sdk-env.sh            # PowerShell: .\scripts\sdk-env.ps1
scripts/preflight-runtime-layout.py --dxmt-runtime "$METALSHARP_DXMT_RUNTIME"
scripts/run-probes.sh --wine "$WINE" --prefix "$WINEPREFIX" --dxmt-runtime "$METALSHARP_DXMT_RUNTIME" --mini-only
```

The macOS package includes the full runtime. On Linux, the contracts, probes, and scripts work against a local Wine (`--wine`, `--prefix`, `--dxmt-runtime`). On Windows, use the probe sources for reference builds.

Build and verify the package:

```bash
tools/dmg/create-bundles.sh
tools/bundles/create-developer-sdk.py --bundle-dir app/bundles --out-dir dist/developer-sdk \
  --manifest dist/bundles/metalsharp-bundle-manifest.tsv
tools/bundles/verify-developer-sdk.sh dist/developer-sdk/metalsharp-d3d12-developer-sdk.tar.zst
```

On `main` and version tags, CI uploads the refreshed tarball and bundle manifest to the `bundles` release. The DMG build then downloads that tarball.
