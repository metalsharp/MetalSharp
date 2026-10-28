# MetalSharp 1.0.0: the Arm64 build with FEX

**Status:** built for MetalSharp 1.0.0 (PR #744). This was the plan, and it stays as the design record. [Architecture](architecture.md#arm64-fex-build) describes the build as it ships. Where 1.0.0 differs from this plan:

- It carries **MetalSharp Wine v4.0** (not 2.1), pinned by `METALSHARP_WINE_TAG` in `release.yml`.
- Each build has its own rule set. The FEX build's `configs/mtsp-rules-fex.toml` is generated from the regular rules, with the games verified on MetalSharp Wine on top.
- Migration takes an explicit path per build and version. A FEX -> FEX update only swaps in the new MetalSharp Wine and runtime pieces. A FEX DMG installed by hand over an x86_64 install runs the full conversion too.
- Games keep their caches per lane, with Clear per lane in Settings. The FEX build shows a one-time welcome after an x86_64 install moves to it.
- A failed FEX build no longer blocks the regular release, and PR CI builds and tests the FEX backend.

## What 1.0.0 ships

There's one GitHub release, `v1.0.0`, with two DMGs:

| DMG | Wine | Runs x86 code with |
|---|---|---|
| `MetalSharp-1.0.0-arm64.dmg` | Today's x86_64 Wine 11.17 runtime. **Behavior is unchanged.** | Rosetta |
| `MetalSharp-1.0.0-arm64-FEX.dmg` | MetalSharp Wine: native arm64 Wine 11.20, with Mullion, WineMetalGL and DXVK | FEX (ARM64EC for x86_64, WoW64 for i386) |

How the two builds relate:

- **One app.** Both builds are the same app: `MetalSharp.app`, bundle id `com.metalsharp.app`, installed in `/Applications`. 0.80.0's `update.sh` checks that bundle id, so the FEX app has to keep it.
- **0.80.0's updater sees both DMGs.** It reads `releases/latest` and treats any `.dmg` with `FEX` in its name as the FEX build; any other `.dmg` is the regular build. Both names contain `arm64`, which it prefers.
- **One switch.** Every change in this plan sits behind a single build switch, the *variant*: `regular` or `fex`. The x86_64 build never takes a `fex` branch.

## 1. Decisions to settle first

1. **Where MetalSharp Wine lives.** Recommended:
   - The FEX DMG carries `MetalSharp-Wine.app`, signed and notarized, unchanged.
   - Setup copies it with `ditto` to `~/.metalsharp/runtime/MetalSharp-Wine.app`.
   - MetalSharp runs it from there.
   - Nothing under it is ever written to or re-signed. Writing would break its seal, and re-signing would drop the cross-architecture entitlement that i386 and 4K pages need, which is tied to its provisioning profile.
   - Everything MetalSharp changes (dxmt.conf, MetalFX, logs) stays outside the bundle.
2. **Prefixes when switching from x86_64 to FEX.** A 0.80.0 user's `prefix-steam` holds their installed Steam games, so throwing it away isn't an option. Options:
   - **(a) Recommended: convert in place, one way.**
     - Back up `system.reg`, `user.reg` and `drive_c/windows`.
     - Run MetalSharp Wine's `wineboot -u`, which installs ARM64X builtins and the FEX WoW64 keys.
     - Write a `.metalsharp-wine-arch` marker of `arm64`.
     - The x86_64 build refuses a prefix marked `arm64`, and the reverse switch restores the backup.
   - **(b) Fresh FEX prefixes**, moving `steamapps` over.
   - Either way it needs real testing (§11). Today nothing records which Wine built a prefix.
3. **Staging DLLs into game folders on FEX.** You asked for staging to keep working when the pipeline is switched.
   - **Mullion:** MetalSharp Wine already serves Mullion from inside the app, ahead of any search path, so a staged copy is redundant. Its Mullion DLLs carry the "Wine builtin" marker, so a copy loads as builtin and is harmless as long as the overrides are builtin-first.
   - **Plan:** keep the mechanics (stage on switch, clean up stale DLLs). On FEX:
     - stage the arm64 lane's DLLs: Mullion from the app's `lib/mullion`, and DXVK from `lib/dxvk` for x86_64 programs;
     - use builtin-first overrides;
     - clean up the x86_64 build's DXMT, D3DMetal and VKD3D copies.
4. **Rosetta on FEX.** Setup no longer installs it, and the macOS 27 Rosetta reinstall is off. A few features still need Rosetta even on FEX:
   - FNA games, which use the x86 Mono except Stardew;
   - the macOS emulators (PCSX2, RPCS3, shadPS4);
   - macOS steamcmd;
   - GPTK (m13).

   Choose between:
   - **(a) Recommended:** ask for Rosetta the first time one of these is used.
   - **(b)** Hide them in the FEX build.

## 2. The variant switch, and the one place that resolves Wine

- **Build time:**
  - The C backend gets `-DMS_BACKEND_VARIANT="fex"` from `make VARIANT=fex`.
  - The app gets a `fex` cargo feature through `METALSHARP_GPUI_CARGO_FEATURES`.
  - `Info.plist` gets `MetalSharpVariant`.
  - The backend reports `variant` from `/status` and `/update/check`.
- **A new `runtime/wine_runtime.c`:** about 50 call sites today hard-code `runtime/wine/bin/metalsharp-wine` and `x86_64-unix`. They change to use it. It returns:
  - the wine, wineserver and `lib` roots;
  - the variant;
  - a `ms_wine_env_apply(route)`.
  - **regular:** exactly today's paths and environment.
  - **fex:** paths inside `MetalSharp-Wine.app`. No `WINEDLLPATH`, no `DYLD_*`, no `VK_ICD_FILENAMES`, no `DXMT_WINEMETAL_UNIXLIB`. `WINESERVER` points at the app's.
  - The call sites are listed by the setup audit, in setup.c, migration.c, steam_actions.c, bottle_actions.c, epic.c, gog.c, ubisoft.c, process.c, sharp.c, gamejolt.c, mono.c, steam.c and diagnostics.c.
- **FEX only:** the startup fixers that write into `runtime/wine` are skipped:
  - the wrapper patcher, SDL symlink, ntdll signing, `xattr -cr` and x87 WoW64 loader symlink;
  - the MoltenVK ICD rewrite;
  - the `metalsharp_ntdll_hook` swap. There's no x64 AppInit DLL in an ARM64X system32.

## 3. Setup and install (FEX)

The setup job (`run_install_all_worker`) gets a FEX step list:

1. Prerequisites, bundled tools, Xcode CLT and zstd, as today. **No Rosetta step.**
2. **Install MetalSharp Wine**, in place of the runtime bundle:
   - copy `MetalSharp-Wine.app` from the DMG's `Resources` to `~/.metalsharp/runtime/`;
   - check it with `codesign --verify --deep --strict` and the team id;
   - run `wine --version`.
3. **Prefix setup, before Steam ever runs.** For `prefix-steam`:
   - `wine wineboot -u`;
   - VC++ 2022 x64 and x86 (`share/metalsharp/redist/vcrun2022`, `/install /quiet /norestart`). They're 32-bit bootstrappers, so they need the signed app;
   - corefonts unpacked with the app's `cabextract`, since their installers fail under Wine;
   - Wine Mono and Gecko come from the app already.

   VC++ moves here from the old after-Steam endpoints and the per-bottle DLL copy.
4. The support assets the FEX build still uses, without the x86_64 runtime:
   - Goldberg, the host runtime ABI, FNA kickstart and shims;
   - the Steam bridge, rules and Mono configs.

   Most of these are already in the app's `share/metalsharp`, so the FEX build reads them there instead of extracting bundles.
5. **FEX readiness checks** instead of the x86_64 ones: the `setup.c` file lists, `runtime_ready()`, `host_runtime_lib_ready`, the MoltenVK hashes and `select_wine_ntdll`.
6. **Dependency list:** no Rosetta row, and MetalSharp Wine is described as native arm64 Wine with FEX, Mullion and DXVK.
7. **Steam install** runs after all of the above, against MetalSharp Wine, in the prepared prefix.

## 4. Steam on FEX

- **The client route:** Wine Steam starts on **`dxmt` (Mullion)**, not `d3dmetal`; on MetalSharp Wine, `d3dmetal` means wined3d. That covers:
  - the route marker;
  - `ensure_wine_steam_pipeline`, and its error text, which names D3DMetal;
  - the Steam handoffs for Marvel Rivals, BG3, EVE, Odyssey and the Rockstar first run.
- **Installing Steam:** SteamSetup is an i386 NSIS installer, so it runs through FEX WoW64 in the signed app. The 32-bit Steam service, overlay and webhelper need the same.
- **Unchanged:** the webhelper wrapper (an x86_64 PE, identical to the app's copy), `seed_steam_registry`, Goldberg, and the Steam desktop redirect.
- **Not on FEX:**
  - the controller shims, because MetalSharp Wine uses its own xinput/dinput over winebus SDL;
  - the FAudio xWMA copy, because MetalSharp Wine has GStreamer and FFmpeg (retest SkyrimSE);
  - the Rosetta AVX and x87 variables.
- **Steam bridge:** decide between MetalSharp's `steambridge.exe` and MetalSharp Wine's `lsteamclient` (patch 0042); test both.

## 5. Graphics routes on FEX

| Route | On FEX |
|---|---|
| `dxmt`, `dxmt_32`, `auto` | Mullion: `GRAPHICS_BACKEND=dxmt`, the app picks its ARM64X or i386 Mullion. Mullion has no D3D12 for i386. |
| `d3dmetal` | Maps to `dxmt`. `GRAPHICS_BACKEND=d3dmetal` is never sent. No `D3DMETAL_*`. |
| `vkd3d` | Maps to `dxmt` for D3D12. VKD3D-Proton isn't bundled. RDR2's `-api Vulkan` doesn't apply. |
| `d3d9` | Wine's d3d9 on WineMetalGL, or DXVK's d3d9 for x86_64 programs. No x87 sidecar. |
| new `dxvk` | `GRAPHICS_BACKEND=dxvk`, x86_64 programs only. |
| `wine_bare`, `m32` | `GRAPHICS_BACKEND=wine`. |
| `m13` (GPTK) | Hidden on FEX, or Rosetta-gated per decision 4. |
| `fna_arm64` | Unchanged (native Mono). Its x86 Mono and shims still need Rosetta. |

Everywhere D3DMetal is forced, FEX maps it to `dxmt`:

- `mtsp_catalog.c` and `configs/mtsp-rules.toml`: 21 games with `pipeline = "d3dmetal"`.
- `ms_steam_detect_graphics_pipeline`, which picks d3dmetal for any d3d12 DLL, and its copies in `steam.c` and `mtsp.c`.
- `ROCKSTAR_LAUNCHER_GAMES`, `D3DMETAL_STEAM_LAUNCHER_GAMES`, Marvel Rivals and BG3, including the BG3 dxmt→d3dmetal migration, which must not run on FEX.
- `PROTECTED_EXE_SWAPS`: Elden Ring, AC6 and GTA V get the stub swap on `dxmt` too.
- `ubisoft.c` `UBISOFT_PIPELINE_DEFAULT`.
- `bottles.c` (the gptk, rosetta and gptk_prefix components), `game.c`, `gamejolt.c`, `epic.c`.
- The app's pipeline lists (`library_model.rs`, `backend.rs`, `sharp_live.rs`, the Sharp tools' "Play D3DMetal") and the Logs filters.

Keep and share: MetalFX and `dxmt.conf` (Mullion reads `DXMT_*` names and `dxmt.conf`), and the shader and pipeline caches, kept apart per variant.

Not on FEX:

- `ensure_dxmt_shader_metal_version`'s writes into the runtime: they move to a `dxmt.conf` under `~/.metalsharp`, passed with `MULLION_CONFIG_FILE`.
- `DXMT_ASYNC_PIPELINE_COMPILE`: Mullion doesn't read it.
- The live MetalFX toggle (it uses `metalsharp_ntdll_hook`): launch-time MetalFX still works. Port the poller to Mullion later.

## 6. WFDXCompat on Mullion (required)

Today WFDXCompat is x86_64-only and wired into the x86_64 Wine's loader through `WFDXCOMPAT_RUNTIME_DIR`, for:

- the Rockstar Games Launcher and SocialClubHelper (a D3D10 bridge, device context state, GDI surfaces);
- GTA V Enhanced (the Agility frontend).

MetalSharp Wine sets `process_uses_wfdx_launchers = FALSE`, and none of the loader hooks are ported.

Work, in `/Volumes/AverySSD/WFDXCompat` and MetalSharp Wine:

1. Build WFDXCompat as **ARM64X** (or ARM64EC) with the arm64 toolchain, plus i386, for the launchers that are 32-bit.
2. **Back it with Mullion:** `wfdx_launcher_init` gets Mullion's `D3D11CreateDevice`.
   - Mullion already has a full D3D10 device, `CreateDeviceContextState` and GDI `GetDC`, so its bridge is a thin pass-through.
   - The Agility frontend's `wfdxbackend-d3d12` points at Mullion's d3d12. Mullion's `D3D12GetInterface` is a stub, which GTA V Enhanced needs for `ID3D12SDKConfiguration`, so it has to be implemented. The NV12 facade is checked too.
3. **Port the WineForge loader hooks** to MetalSharp Wine, gated on Mullion instead of D3DMetal:
   - `graphics_policy.c` and `wfdxcompat_loader.c`;
   - the d3d10core `__wine_get_wfdx_d3d10_create_device` dispatch;
   - the export interposition and the alias.
4. **Ship it** in MetalSharp Wine (`lib/wfdxcompat`), so MetalSharp only sets `WFDXCOMPAT_RUNTIME_DIR` into the app.
5. **Ubisoft Connect** (i386, `upc.exe`) on Mullion i386, plus whatever WFDXCompat needs for it. Its wined3d GPU-ID registry trick for DataDome login doesn't apply on Mullion, so the login is retested.

## 7. Other launchers and prefixes on FEX

All of these go through `wine_runtime.c`, with `wineboot -u` and `wineserver` from the app:

- **Epic** (`bottles/epic_*`), **GOG** (`gog-prefix`), **Game Jolt**, the **Sharp Library** (bottles, `sharp-prefix`), **Ubisoft** (`prefix-ubisoft`, on `dxmt`).
- Prefix setup for each: the same `wineboot -u` + VC++ + corefonts step as `prefix-steam`, so each works the first time.
- **Fonts:** `ensure_prefix_fonts` reads the app's `share/wine/fonts`; the app's wine.inf already installs them.
- **innoextract:** comes from the app's `Contents/MacOS`.
- **Wine Mono:** comes from the app, so `mono.c`'s msi install is skipped.
- **`bottle_actions.c`:**
  - `seed_vcpp_runtime_dlls` (x86_64 DLLs into an ARM64X system32) is skipped; the bundled VC++ installer replaces it;
  - win32 profiles are relabelled, since MetalSharp Wine is WoW64 only;
  - the DXMT win32 repair is dropped.
- **`DYLD_INSERT_LIBRARIES` is stripped** by the hardened app. The Elden Ring P-core shim needs `allow-dyld-environment-variables` added to MetalSharp Wine's signing, or the CPU limit done in Wine. Dock icons need `LSUIElement` in MetalSharp Wine's `Info.plist`; `dockhide.c` is dead code.
- **Process detection** (`process.c`, `steam_actions.c`, `ubisoft.c`, `sharp.c`, `gamejolt.c`): the "under `runtime/wine`" checks also accept the app's real path (`…/MetalSharp-Wine.app/Contents/{MacOS,lib/wine/aarch64-unix}/wine`). Every `wineserver -k` uses the app's wineserver.

## 8. Migration and updates

- **Recording the variant:**
  - `setup.json` records `runtime_variant` and the MetalSharp Wine version;
  - `MIGRATION_SCHEMA` is bumped;
  - `/update/check` reports the installed `variant`.
- **Migration (FEX):**
  - re-copies MetalSharp Wine instead of extracting bundles;
  - converts or recreates prefixes per decision 2;
  - runs `wineboot -u` with the app's Wine on every preserved prefix, not just GOG and Ubisoft.
- **Updater:**
  - The FEX app's updater stays on FEX by default and shows it as "current".
  - Switching builds at the **same version** has to be allowed. `update.sh`'s `version_gt` refuses it today (`target_not_newer`).
  - The FEX install path gets the same Developer ID and team check the regular recovery path has.
  - A newer MetalSharp Wine arrives inside a newer FEX DMG.
- **Release asset names stay:** `…-arm64.dmg` for regular, which `update.sh --recover` depends on, and `…-arm64-FEX.dmg`.

## 9. App UI (FEX)

- **Pipeline menus** (hero card, collection, Sharp engines): offer Mullion (DXMT), DXVK, D3D9 and Wine. No D3DMetal, VKD3D or M13.
- **Settings and dependencies:** no x87 acceleration row; no Rosetta row; Backend shows "MetalSharp Wine (Arm64, FEX)".
- **Setup wizard text:** no GPTK or runtime-bundle wording; it says what FEX and Mullion are.
- **Updates:** the update dialog marks the current build.

## 10. Release CI

- **The x86_64 build job is unchanged.**
- **A new `build-fex` job (macos-26):**
  - downloads `MetalSharp-Wine-<ver>-arm64.zip` from a pinned metalsharp/MetalSharp-Wine release (`METALSHARP_WINE_TAG`, v4.0 for 1.0.0: `MetalSharp-Wine-4.0-arm64.zip`) and checks it against `SHA256SUMS`;
  - builds the app and backend with `VARIANT=fex`;
  - puts `MetalSharp-Wine.app` in `Resources/`, excluded from the re-sign loop so its signature, entitlements and profile stay intact;
  - packages `MetalSharp-<ver>-arm64-FEX.dmg`, then signs, notarizes and staples it;
  - runs its own verify script (MetalSharp Wine present and valid; no x87sidecar or D3DMetal requirement).
- **The `release` job** uploads both DMGs to the same `v<ver>` release. A failed `build-fex` leaves the FEX DMG out instead of blocking the regular one.
- **MetalSharp Wine 2.1** ships first, with: the 12_2 Mullion build, WFDXCompat on Mullion, `LSUIElement`, the DYLD entitlement and helper entry points as needed.

## 11. Local testing (before any release)

- Build MetalSharp Wine 2.1 locally and the FEX DMG locally.
- Install it over a real 0.80.0 x86_64 install, and also fresh.
- Check:
  - setup with no Rosetta;
  - `prefix-steam` prepared with VC++ before Steam;
  - Steam installs and logs in, and CEF works;
  - a Steam game on Mullion D3D11 and D3D12, a DXVK title, and a 32-bit title;
  - pipeline switching, with staging and cleanup;
  - the Rockstar Games Launcher with RDR2, and GTA V Enhanced, through WFDXCompat on Mullion;
  - Ubisoft Connect installs, logs in and launches a game;
  - Epic and GOG prefixes and a launch each;
  - a Sharp Library installer (Moonscraper);
  - migration from 0.80.0, with existing Steam games still installed;
  - switching back to the x86_64 build;
  - the in-app updater offering both builds.
- The full x86_64 suite and a regular x86_64 DMG must behave exactly as 0.80.0 does.

## Order of work

1. MetalSharp Wine 2.1: entry points, signing changes, and WFDXCompat on Mullion (§6, §7).
2. The variant switch and `wine_runtime.c` (§2), with the x86_64 paths byte-for-byte unchanged and the backend tests green.
3. FEX setup, prefixes and Steam (§3, §4).
4. FEX graphics routes and the D3DMetal mapping (§5).
5. Launchers and prefixes (§7).
6. Migration and the updater (§8).
7. App UI (§9).
8. Release CI for the FEX DMG (§10).
9. Local testing until everything passes (§11). Then, and only then, `v1.0.0`.
