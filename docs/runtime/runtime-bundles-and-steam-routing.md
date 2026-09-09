# Runtime Bundles and Steam Routing
**Updated:** 2026-09-08


This is the operational contract for bundle provenance and Wine Steam launch routing.

## Bundle Provenance

Runtime assets are downloaded from the `bundles` GitHub release into `app/bundles/` during app packaging and into `~/.metalsharp/cache/bundles/` during installer fallback downloads.

The manifest-tracked assets are listed in `tools/bundles/asset-manifest.tsv`. The verifier checks archive roots, required files, and lane-specific hash contracts. Release staging also verifies downloaded archive bytes against the published bundle manifest; replacing an asset requires updating that manifest, not bypassing checks.

Current split bundle roots:

| Asset | Why it is guarded |
|---|---|
| `metalsharp-electron.tar.zst` | Contains `electron/`, the built Electron application payload. |
| `metalsharp-graphics-dll.tar.zst` | Contains `Graphics/dll/`, the legacy DXMT D3D9/D3D10/D3D11 surface and the isolated M12 D3D12 surface. |
| `metalsharp-runtime.tar.zst` | Contains `runtime/`, the patched Wine 11.17 runtime, host ABI, and managed runtime payloads including D3DMetal. |
| `metalsharp-assets.tar.zst` | Contains `assets/`, Mono, Goldberg, EAC toggle, shims, and compatibility/runtime support assets. |
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

The installer consumes the split runtime tarballs by root name. `metalsharp-graphics-dll.tar.zst` is the only source for the active DXMT runtime payloads used by M9-M12.

The graphics bundle has two runtime surfaces:

```text
Graphics/dll/dxmt/      -> DXMT v0.80 baseline and retained compatibility payloads
Graphics/dll/dxmt-m12/  -> isolated D3D12/DXGI/winemetal payload for M12
```

After install those surfaces live under:

```text
~/.metalsharp/runtime/wine/lib/dxmt/
~/.metalsharp/runtime/wine/lib/dxmt_m12/
```

Installed DXMT runtime state is recorded in:

```text
~/.metalsharp/runtime/wine/lib/dxmt/metalsharp-dxmt-runtime.json
```

Both installed lanes have `metalsharp-dxmt-runtime.json` metadata using schema `metalsharp.dxmt-runtime.v2`; for app 0.65.0 the baseline version is `0.65.0-dxmt-v0.80-baseline-v1`. Migration must accept the same version that setup writes, not the retired M12 manifest suffix.

Do not trust Wine or DXMT version strings alone. Check required files, both lane manifests, and the M12/DXVK/VKD3D/MoltenVK hash contracts when diagnosing deployment drift. The bundled backend is packaged separately at `Contents/Resources/runtime/metalsharp-backend`.

D3DMetal uses `~/.metalsharp/runtime/d3dmetal-gptk4-beta2/` with the same Wine 11.17 host and Steam prefix, not a Homebrew-owned runtime. See [Wine Architecture](wine-architecture.md#d3dmetal).

## Steam Launch Route

The app launches Wine Steam through:

```text
Renderer button -> POST /steam/launch -> steam::launch_wine_steam()
```

Game launches that need an explicit public route use M12/M11/M10/M9/VKD3D/D3DMetal/Mono-FNA route IDs. Raw `dxmt` remains an internal auto-router and legacy compatibility value.

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

Before launching Steam, MetalSharp calls `ensure_steam_launch_ready()` and redeploys `steamwebhelper.exe` when Steam has overwritten it. Steam assets come from `metalsharp-steam.tar.zst`, and the C backend validates the staged wrapper before launch.

The expected deployed Steam CEF layout is:

```text
~/.metalsharp/prefix-steam/drive_c/Program Files (x86)/Steam/bin/cef/cef.win64/
├── steamwebhelper.exe       # MetalSharp wrapper
├── steamwebhelper_real.exe  # Steam's original helper
└── .ms_wrapper_deployed
```

If Steam login stops rendering after bundle or wrapper work, verify the wrapper hash before changing launch args.
