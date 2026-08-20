# Runtime Bundles and Steam Routing
**Updated:** 2026-07-08


This is the operational contract for bundle provenance and Wine Steam launch routing.

## Bundle Provenance

The shipped MetalSharp DMG intentionally contains only two bootstrap archives:

| Asset | Purpose |
|---|---|
| `metalsharp-steam.tar.zst` | Steam installer and Steam CEF wrapper assets. |
| `goldberg.tar.zst` | Goldberg x86/x64 DLLs used by offline/Goldberg routes. |

The complete cross-architecture Wine runtime is **not** embedded in the DMG.
After the app is installed, `scripts/tools/migrator/migrate-to-vkmt-runtime.sh`
downloads the pinned VKMT-Wine release installer, verifies it, and installs
the current VKMT runtime before creating or migrating `prefix-steam`.

The old split archives remain useful as release/build inputs and compatibility
fallbacks, but they are not copied into the DMG.

The manifest-tracked assets are listed in `tools/bundles/asset-manifest.tsv`. The verifier checks that each tarball exists and contains the expected baby-named root.

Legacy split bundle roots used by the build/release tooling:

| Asset | Why it is guarded |
|---|---|
| `metalsharp-electron.tar.zst` | Contains `electron/`, the built Electron application payload. |
| `metalsharp-graphics-dll.tar.zst` | Contains `Graphics/dll/`, the legacy DXMT D3D9/D3D10/D3D11 surface and the isolated M12 D3D12 surface. |
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
tools/dmg/verify-dmg-runtime-assets.sh dist/MetalSharp.dmg
```

## Update and first-install flow

1. The updater downloads and installs the latest MetalSharp DMG.
2. The newly installed app sees the post-update migration marker.
3. The migration handler runs the VKMT migration script.
4. The script downloads and verifies VKMT-Wine, replaces the runtime, and
   creates a fresh VKMT `prefix-steam`.
5. Existing Steam files, libraries, settings, caches, UI storage, and drive
   mappings are restored; bottles are not copied.

First install uses the same script. With no existing `prefix-steam`, it
wineboots a new prefix, installs Steam from `metalsharp-steam.tar.zst`, and
stages Goldberg from `goldberg.tar.zst`.

## Installer Acceptance Rules

The installer consumes the split runtime tarballs by root name. `metalsharp-graphics-dll.tar.zst` is the only source for the active DXMT runtime payloads used by M9-M12.

The graphics bundle has two runtime surfaces:

```text
Graphics/dll/dxmt/      -> legacy DXMT payload for M9, M10, and M11
Graphics/dll/dxmt-m12/  -> updated D3D12/DXGI/winemetal payload for M12 only
```

After install those surfaces live under:

```text
~/.metalsharp/runtime/wine/lib/dxmt/
~/.metalsharp/runtime/wine/lib/dxmt-m12/
```

Installed DXMT runtime state is recorded in:

```text
~/.metalsharp/runtime/wine/lib/dxmt/metalsharp-dxmt-runtime.json
```

Do not trust a runtime by version string alone. Check the manifest, required DLLs, the `dxmt-m12` sidecars, and source archive hash when diagnosing deployment drift.

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
