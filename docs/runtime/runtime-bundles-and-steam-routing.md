# Runtime Bundles and Steam Routing
**Updated:** 2026-09-22

## Bundle Provenance

Runtime assets download from the `bundles` GitHub release into `app/bundles/` during app packaging and into `~/.metalsharp/cache/bundles/` during installer fallback downloads.

The manifest-tracked assets are listed in `tools/bundles/asset-manifest.tsv`. The verifier checks archive roots, required files, and lane-specific hash contracts. Release staging verifies downloaded archive bytes against the published manifest; replacing an asset updates that manifest.

| Asset | Contents |
|---|---|
| `metalsharp-electron.tar.zst` | `electron/`, the built Electron application payload |
| `metalsharp-graphics-dll.tar.zst` | `Graphics/dll/`, the DXMT baseline surface and the isolated D3D12 surface |
| `metalsharp-runtime.tar.zst` | `runtime/`, the patched Wine 11.17 runtime, host ABI, and managed payloads including D3DMetal |
| `metalsharp-assets.tar.zst` | `assets/`, Mono, Goldberg, EAC toggle, shims, and compatibility support |
| `metalsharp-scripts-tools.tar.zst` | `scripts/tools/`, updater scripts, configs, native tools, CEF helpers |
| `metalsharp-steam.tar.zst` | `steam/`, the Steam installer and Steam CEF wrapper |
| `metalsharp-d3d12-developer-sdk.tar.zst` | `developer-sdk/d3d12/`, D3D12 contracts, probes, scripts, docs, staged Wine runtime, DXMT DLLs, winemetal bridge files |

Verification commands:

```bash
tools/bundles/verify-bundles.sh --require mac
tools/bundles/verify-bundles.sh --release
tools/bundles/verify-developer-sdk.sh app/bundles/metalsharp-d3d12-developer-sdk.tar.zst
```

## Installer Acceptance

The installer consumes the split tarballs by root name. `metalsharp-graphics-dll.tar.zst` is the only source for the active DXMT payloads:

```text
Graphics/dll/dxmt/      -> DXMT v0.80 baseline and retained compatibility payloads
Graphics/dll/dxmt-m12/  -> isolated D3D12/DXGI/winemetal payload
```

After install those surfaces live under:

```text
~/.metalsharp/runtime/wine/lib/dxmt/
~/.metalsharp/runtime/wine/lib/dxmt_m12/
```

Both lanes record installed state in `metalsharp-dxmt-runtime.json` (schema `metalsharp.dxmt-runtime.v2`; baseline `0.65.0-dxmt-v0.80-baseline-v1`). Diagnose deployment drift by checking required files, both lane manifests, and the hash contracts — version strings alone do not identify the runtime.

The bundled backend is packaged separately at `Contents/Resources/runtime/metalsharp-backend`.

## Steam Launch Route

Wine Steam launches through:

```text
Renderer button -> POST /steam/launch -> steam::launch_wine_steam()
```

The backend launches Wine Steam so it gets the managed prefix, runtime library env, DLL overrides, and wrapper deployment.

Game launches use the public route IDs:

```text
Renderer Play -> POST /steam/launch-game {"launchMethod":"dxmt"} -> prepare_steam_pipeline_env() -> direct game launch with Wine Steam alive in the background
```

Steam-model titles stage the real Steam API DLLs and overlay components next to the selected executable when available:

```text
steam_api64.dll
steam_api.dll
steamclient64.dll
steamclient.dll
GameOverlayRenderer64.dll
GameOverlayRenderer.dll
```

## Steam Wrapper

Before launching Steam, MetalSharp calls `ensure_steam_launch_ready()` and redeploys `steamwebhelper.exe` when Steam has overwritten it. Steam assets come from `metalsharp-steam.tar.zst`, and the backend validates the staged wrapper before launch:

```text
~/.metalsharp/prefix-steam/drive_c/Program Files (x86)/Steam/bin/cef/cef.win64/
├── steamwebhelper.exe       # MetalSharp wrapper
├── steamwebhelper_real.exe  # Steam's original helper
└── .ms_wrapper_deployed
```

If Steam login stops rendering after bundle or wrapper work, verify the wrapper hash before changing launch args.
