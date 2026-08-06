# Runtime Bundles and Steam Routing
**Updated:** 2026-07-08


This is the operational contract for bundle provenance and Wine Steam launch routing.

## Bundle Provenance

Runtime assets are downloaded from the `bundles` GitHub release into `app/bundles/` during app packaging and into `~/.metalsharp/cache/bundles/` during installer fallback downloads.

The manifest-tracked assets are listed in `tools/bundles/asset-manifest.tsv`. The verifier checks that each tarball exists and contains the expected baby-named root.

Current split bundle roots:

| Asset | Why it is guarded |
|---|---|
| `metalsharp-electron.tar.zst` | Contains `electron/`, the built Electron application payload. |
| `metalsharp-graphics-dll.tar.zst` | Contains `Graphics/dll/`, the DXMT surfaces (legacy `dxmt` for M9/M10/M11 + `dxmt-m12` M12 rollback) and the vkd3d-proton M12 stack lanes (`vkd3d-proton`, `dxvk`, `moltenvk-vkmt`). |
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

The installer consumes the split runtime tarballs by root name. `metalsharp-graphics-dll.tar.zst` is the only source for the active graphics runtime payloads used by M9-M12.

The graphics bundle has five runtime surfaces:

```text
Graphics/dll/dxmt/           -> legacy DXMT payload for M9, M10, and M11
Graphics/dll/dxmt-m12/       -> DXMT M12 rollback payload (also supplies the
                               shared nvapi64/nvngx GPU vendor stubs)
Graphics/dll/vkd3d-proton/   -> default M12 D3D12 stack (d3d12.dll + d3d12core.dll)
Graphics/dll/dxvk/           -> shared D3D9/D3D10/D3D11/DXGI surface; dxgi.dll for M12
Graphics/dll/moltenvk-vkmt/  -> VKMT patched MoltenVK (libMoltenVK.dylib + ICD)
```

After install those surfaces live under:

```text
~/.metalsharp/runtime/wine/lib/dxmt/
~/.metalsharp/runtime/wine/lib/dxmt-m12/
~/.metalsharp/runtime/wine/lib/vkd3d-proton/
~/.metalsharp/runtime/wine/lib/dxvk/
~/.metalsharp/runtime/wine/lib/moltenvk-vkmt/
```

Installed DXMT runtime state is recorded in:

```text
~/.metalsharp/runtime/wine/lib/dxmt/metalsharp-dxmt-runtime.json
```

Do not trust a runtime by version string alone. Check the manifest, required DLLs, the vkd3d-proton/DXVK/MoltenVK lane artifacts (for the default M12 backend) or the `dxmt-m12` sidecars (for the DXMT rollback), and source archive hash when diagnosing deployment drift.

## Steam Launch Route

The app launches Wine Steam through:

```text
Renderer button -> POST /steam/launch -> steam::launch_wine_steam()
```

Game launches that need an explicit public route use M12/M11/M10/M9/Mono-FNA route IDs. Raw `dxmt` remains an internal auto-router and legacy compatibility value.

```text
Renderer Play -> POST /steam/launch-game {"launchMethod":"m12"} -> prepare_steam_pipeline_env() -> direct game launch with Wine Steam alive in the background
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
