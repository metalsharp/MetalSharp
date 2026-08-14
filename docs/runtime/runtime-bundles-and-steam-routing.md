# Runtime Bundles and Steam Routing
**Updated:** 2026-08-11


This is the operational contract for bundle provenance and Wine Steam launch routing.

## Bundle Provenance

Runtime assets are downloaded from the `bundles` GitHub release into `app/bundles/` during app packaging and into `~/.metalsharp/cache/bundles/` during installer fallback downloads.

Every archive is published with its SHA-256 in the `metalsharp-bundle-manifest.tsv` asset of the `bundles` release. Consumers that execute bundle payloads pin those digests rather than trusting the download channel: the production installer (`scripts/install-metalsharp-wine-runtime.sh`) pins its archive digests inline, and the CI VKD3D gate (`tools/ci/vkd3d-check.sh`) verifies each downloaded bundle against the pinned manifest `tools/ci/vkd3d-bundle-hashes.tsv` via `tools/ci/verify-bundle-sha256.sh`, failing the gate on any mismatch. Update that manifest in the same change that rotates the `bundles` release.

The manifest-tracked assets are listed in `tools/bundles/asset-manifest.tsv`. The verifier checks that each tarball exists and contains the expected baby-named root.

Current split bundle roots:

| Asset | Why it is guarded |
|---|---|
| `metalsharp-electron.tar.zst` | Contains `electron/`, the built Electron application payload. |
| `metalsharp-graphics-dll.tar.zst` | Contains `Graphics/dll/`, the DXMT surfaces (`dxmt`) and the VKD3D stack lanes (`vkd3d-proton`, `dxvk`, `moltenvk-vkmt`). |
| `metalsharp-runtime.tar.zst` | Contains `runtime/`, the Wine runtime, host ABI, and backend executable. |
| `metalsharp-assets.tar.zst` | Contains `assets/`, Mono, GPTK, DXVK, Goldberg, EAC toggle, shims, and runtime support assets. |
| `metalsharp-scripts-tools.tar.zst` | Contains `scripts/tools/`, updater scripts, configs, native tools, and CEF helpers. |
| `metalsharp-steam.tar.zst` | Contains `steam/`, the Steam installer and Steam CEF wrapper assets. |
| `metalsharp-d3d12-developer-sdk.tar.zst` | Contains `developer-sdk/d3d12/`, the D3D12 contracts, probes, scripts, docs, staged developer Wine runtime, DXMT DLLs, Winemetal bridge files, and runtime provenance manifest. |

Verification commands:

```bash
tools/bundles/verify-bundles.sh --require mac
tools/bundles/verify-bundles.sh --release
tools/bundles/verify-developer-sdk.sh app/bundles/metalsharp-d3d12-developer-sdk.tar.zst
```

## Installer Acceptance Rules

The installer consumes the split runtime tarballs by root name. `metalsharp-graphics-dll.tar.zst` is the only source for the active graphics runtime payloads used by VKD3D/DXMT.

The graphics bundle has five runtime surfaces:

```text
Graphics/dll/dxmt/           -> DXMT payload for DXMT/DXMT(32)
Graphics/dll/dxmt-vkd3d/       -> DXMT VKD3D rollback payload (also supplies the
                               shared nvapi64/nvngx GPU vendor stubs)
Graphics/dll/vkd3d-proton/   -> default VKD3D D3D12 stack (d3d12.dll + d3d12core.dll)
Graphics/dll/dxvk/           -> shared D3D9/D3D10/D3D11/DXGI surface; dxgi.dll for VKD3D
Graphics/dll/moltenvk-vkmt/  -> VKMT patched MoltenVK (libMoltenVK.dylib + ICD)
```

After install those surfaces live under:

```text
~/.metalsharp/runtime/wine/lib/dxmt/
~/.metalsharp/runtime/wine/lib/dxmt-vkd3d/
~/.metalsharp/runtime/wine/lib/vkd3d-proton/
~/.metalsharp/runtime/wine/lib/dxvk/
~/.metalsharp/runtime/wine/lib/moltenvk-vkmt/
```

Installed DXMT runtime state is recorded in:

```text
~/.metalsharp/runtime/wine/lib/dxmt/metalsharp-dxmt-runtime.json
```

Do not trust a runtime by version string alone. Check the manifest, required DLLs, the vkd3d-proton/DXVK/MoltenVK lane artifacts (for the default VKD3D backend) or the `dxmt-vkd3d` sidecars (for the DXMT rollback), and source archive hash when diagnosing deployment drift.

## Downloaded Installer Artifact Integrity

Every downloaded installer that can reach an install or privileged update path
must have a non-zero expected size and a SHA-256 digest before it is accepted:

- Update DMGs use the GitHub release asset digest and URL/size/hash sidecar in
  `~/.metalsharp/cache/updates/`. A cache entry is usable only when the
  sidecar exactly matches the current release metadata and the DMG is hashed
  again.
- Wine Mono 11.2.0 uses the pinned MSI size and SHA-256 in
  `app/src-rust/src/mono.rs`. Partial or mismatched `.msi.part` files are
  removed before retrying.
- SteamSetup.exe is staged only from the verified `metalsharp-steam` bundle;
  the bundle archive and extracted installer are both pinned by size and
  SHA-256 in `app/src-rust/src/steam.rs`.

The detached updater receives the DMG size and digest from the backend and
rechecks them immediately before mounting. `hdiutil verify` remains an
additional structural check, not a replacement for the content hash.

## Steam Launch Route

The app launches Wine Steam through:

```text
Renderer button -> POST /steam/launch -> steam::launch_wine_steam()
```

Game launches that need an explicit public route use VKD3D/DXMT/DXMT(32)/Mono-FNA route IDs. Raw `dxmt` remains an internal auto-router and legacy compatibility value.

```text
Renderer Play -> POST /steam/launch-game {"launchMethod":"vkd3d"} -> prepare_steam_pipeline_env() -> direct game launch with Wine Steam alive in the background
```

Wine Steam must be launched by the backend so it gets the managed Wine prefix, runtime library env, DLL overrides, and wrapper deployment. Launching `Steam.exe` directly from a shell is not equivalent to pressing the app button.

Steam-model titles use the real Steam DLLs instead of Goldberg. For those titles the launcher stages the real Steam API DLLs and the Steam client/overlay components next to the selected executable when they are available:

```text
steam_api64.dll
steam_api.dll
steamclient64.dll
steamclient.dll
GameOverlayRenderer64.dll
GameOverlayRenderer.dll
```

This applies to Steam launch-model titles such as Party Animals and Source games without forcing `-secure` onto titles that only need `-steam`.

## Steam Wrapper Rules

Before launching Steam, MetalSharp calls `ensure_steam_launch_ready()` and redeploys `steamwebhelper.exe` when Steam has overwritten it. Steam assets come from `metalsharp-steam.tar.zst`, and the backend only accepts the extracted wrapper if it matches `STEAMWEBHELPER_WRAPPER_SHA256` in `app/src-rust/src/steam.rs`.

The expected deployed Steam CEF layout is:

```text
~/.metalsharp/prefix-steam/drive_c/Program Files (x86)/Steam/bin/cef/cef.win64/
├── steamwebhelper.exe       # MetalSharp wrapper
├── steamwebhelper_real.exe  # Steam's original helper
└── .ms_wrapper_deployed
```

If Steam login stops rendering after bundle or wrapper work, verify the wrapper hash before changing launch args.
