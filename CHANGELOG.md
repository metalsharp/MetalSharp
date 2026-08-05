# Changelog

## Unreleased

### Fixed

- **Legacy OpenGL games under Wine** — stop forcing `MS_FWD_COMPAT_GL_CTX=1` globally for Steam, GOG prefix initialization, and the Wine wrapper. Games now receive compatibility-profile semantics by default, while route-specific environments can still opt into forward-compatible contexts. Steam also disables Wine tracing by default instead of logging every OpenGL call; `METALSHARP_WINEDEBUG` remains available for explicit diagnostics. This fixes black output and unstable frame pacing in games such as The Binding of Isaac: Repentance.

## v0.54.5 — 2026-07-09

Testing-surface hardening, pre-commit strictness, and compatibility database refresh.

### Added

- **`tools/ci/validate-rules-toml.py`** — lightweight Python validator for `configs/mtsp-rules.toml`. Catches: TOML parse errors, duplicate `[overrides.APPID]` sections, missing/empty `name`, missing/unknown `pipeline`, unrecognized sub-table keys. Runs in CI without the Rust toolchain.
- **`tools/ci/check-doc-freshness.py`** — warns on docs without an `Updated:` header or older than 120 days; also verifies `CHANGELOG.md` has a section for the current version. Warn-only by default; `--strict` makes it an error.
- **`.github/hooks/pre-commit`** + README — opt-in shared pre-commit hook. Runs `cargo fmt --check`, `cargo clippy -- -D warnings`, `cargo test --lib`, `clang-format --dry-run --Werror`, `tsc --noEmit`, `biome ci`, `prettier --check`, and the rules-TOML/doc-freshness validators locally when relevant files are staged.
- **`shipped_rules_toml_is_well_formed`** Rust test — same checks as the Python rules TOML validator, runs as part of `cargo test`.
- **609 default rule entries** for Steam AppIDs — bulk-add of game compatibility rules for DX9/10/11/12 pipelines, sourced from Steam store data and community testing. Pipeline distribution: m9: 54, m10: 2, m11: 471, m11_32: 70, m12: 12.

### Changed

- **Pre-commit policy: fail-hard on missing toolchains.** Previously the pre-commit hook would warn and silently skip a check if the required toolchain (cargo, clang-format, node_modules, python3) wasn't installed. That made it possible for a `cargo fmt` violation to slip past locally and only get caught in CI. The hook now fails the commit in that case, with an install hint.
- **Compatibility database** — `docs/compatibility/GAMES-SUPPORTED.md` now documents the M9, M10, M11, M11-32, M12, and D3DMetal pipeline coverage and corrects the Party Animals AppID (1260320; the prior 1823720 was incorrect).

### Fixed

- **Party Animals AppID correction** — was 1823720 (Mail Mole); correct ID is 1260320.
- **rustfmt violation** in the new `shipped_rules_toml_is_well_formed` test caught in CI, fixed by collapsing the multi-line chain call.

### Documentation

- Added `**Updated:** 2026-07-08` headers to 37 docs that lacked a date stamp.
- Archived 4 dead/historical roadmaps to `docs/archive/roadmaps/` (`metalsharp-final-roadmap.md`, `dx12-pipeline-complete-roadmap.md`, `beta7-dxmt-cohesion-roadmap.md`) and 1 phase PR summary to `docs/archive/optimization-roadmap/`. New `docs/archive/README.md` documents the archive policy.
- Removed `docs/compatibility/game-compat.md` (a 4-line redirect stub pointing to GAMES-SUPPORTED.md).

## v0.54.1 — 2026-07-08

M11(32)/M10(32) bottle visual fix, GOG/OAuth hardening, Wine Mono bundling, Steam DLL cache, and D3DMetal save feedback.

### Fixed

- **M11(32)/M10(32) bottle visual dropdown** — (#256, #258) bottles and library cards now correctly surface the 32-bit variants; the dropdown expands only the active row, and `(32)` annotations appear in the right places.
- **GOG OAuth** — (#258, #259) replace the Safari AppleScript browser polling with a bundled Electron `BrowserWindow` helper. Restores the app icon on the OAuth window and surfaces GOG as a fully-supported library source.
- **Wine Mono install** — (#258) bundled wine-mono-11.2.0 installs asynchronously with progress; old mono is cleaned up before reinstall; `wine-mono-11.2.0.marker` written on success.
- **Steam DLL cache** — (#258) for `steam_interfaces.txt` Goldberg toggle, ensure DLLs are present and the cache is regenerated if empty/incomplete.
- **D3DMetal save feedback** — (#258) library cards now show the actual D3DMetal bottle state on save; stale manifests are normalized.
- **`winemetal.so` deploy** — removed an incorrect copy of `winemetal.so` to `system32` during i386 repair that was being overwritten on every save.

### Documentation

- **README** — GOG compatibility section, Discord badge, and a few wording passes.

## v0.53.5 — 2026-07-08

M11(32)/M10(32) bottle save correctness.

### Fixed

- **Bottle save / DLL surface for 32-bit pipelines** — the i386 DXMT lanes now save bottles correctly. The previous implementation swapped DLLs but did not rebind the bottle manifest to the right `bottle_id`, so saves appeared to succeed but pointed at the wrong `drive_c` snapshot. Also resolves the `(32)` exe-resolution edge case where the loader would pick the 64-bit exe on a 32-bit pipeline.

## v0.53.0 — 2026-07-08

32-bit DXMT routes for M11/M10, MetalFX live toggle, and Process Manager GPU overlay.

### Added

- **M11(32) and M10(32) DXMT routes** — (#253) 32-bit D3D11 and D3D10 games now route through the i386 DXMT lanes. The new `M11_32` and `M10_32` PipelineId values are wired through the rules engine, the bottle doctor, and the graphics runtime repair path.
- **M11(32)/M10(32) staging + doctor** — the i386 DXMT lanes are staged alongside the existing x86_64 lanes, and the bottle doctor checks the right DLLs for each variant.
- **Process Manager performance overlay** — ported from the 0.51 branch, with an interactive vcrun2019 repair button and honest CPU temperature reporting (no more `kIO*` shim).
- **MetalFX Spatial Upscaling live toggle** — in the Process Manager overlay, the user can flip MetalFX on/off per-process without restarting.
- **GPU load overlay** — the Process Manager now reports real GPU utilization, not the placeholder value.

### Fixed

- **Hades + Titan Quest through M11(32)** — (#254) Hades and Titan Quest now resolve to the `(32)` exe and the bottle DLL surface checks the right DXMT files.
- **GOG migration metadata** — preserves GOG bottle metadata across migrations.
- **CI bundle hook list** — adds a missing line continuation in `verify-bundles.sh`'s runtime hook list.

## v0.51.0 — 2026-07-01

GOG MetalSharp Games launcher, M12 release path, D3DMetal GPTK explicit lane, dependency hygiene, and Electron theme polish.

### Added

- **GOG MetalSharp Games launcher** — (#242) a fully separate GOG launcher built on the `gogdl` downloader. GOG OAuth now flows through a bundled Electron `BrowserWindow` (replacing the previous Safari AppleScript bridge). GOG prefixes are managed end-to-end, GOG cards get artwork, and the GOG install path is shared with the Steam path for compatibility surface purposes.
- **M12 release runtime path** — release CI now installs the DXMT build tools and MinGW toolchain and verifies the M12 archive layout. The `chore: bump release version` script verifies all 5 version locations are in sync.
- **D3DMetal GPTK explicit lane** — #231 adds a dedicated `D3DMetal` lane separate from `M12`, with its own bottle repair actions, runtime staging, and Titan Quest M9 rule.
- **D3DMetal → M12 route switching** — bottles can be switched between D3DMetal and M12 with the right DLLs swapped automatically.
- **M12 winemetal sidecar validation** — release CI validates the staged `winemetal.dylib` and `winemetal.so` sidecars.
- **Process Manager performance + process controls** — the in-game overlay now has explicit performance and process controls (per-process kill, GPU usage, etc.).
- **Developer theme preview** — a new developer theme that uses neutral sidebar active text and an honest library card grid.

### Changed

- **Library card grid** — cards now lay out in a fixed 2-column grid; glass sidebar active route shimmers on route change; the developer theme is opt-in.
- **Dependency bumps** — bumps `vue`, `lucide-icons`, `biome`, `electron`, and the `sha2` Rust crate. Patches vulnerable npm transitive deps.
- **CI: CodeQL C** — restored and then removed (job was kept active in the meantime).
- **Subnautica 2** — now launches directly on M12 (no more guard).

### Fixed

- **M12 normal launch pipeline** — guarded staging for normal launches (not just Steam).
- **M12 Steam launch artifact staging** — guarded.
- **GOG OAuth callback capture** — OAuth callback tabs are now captured correctly.
- **GOG prefix setup state refresh** — GOGDL is provisioned during prefix setup.
- **GOG uninstall + retry state** — hardened.
- **Bottle profile save isolation** — saves no longer share state between D3DMetal and M12.
- **M12 runtime repair contract** — CI checks added.
- **M12 unix sidecar staging** — staged for game launches.
- **M12 command/present milestone logging** — milestones are now logged in the runtime.

## v0.50.0 — 2026-06-13

M12 DXIL vertex input hardening, RE4 diagnostic capture, and the M12 cube pipeline CI gate.

### Added

- **M12 DXIL vertex input mapping** — uses vertex pulling for DXIL vertex inputs (replaces an earlier "share unix winemetal" attempt that was reverted). The shared IA metadata builder is now used by both the M12 cube runner and the game launch path.
- **M12 fragment bindings hardened** — compute binding completeness logs added; zero-draw swapchain presents classified; direct swapchain clear work recorded; command list lifecycle traced.
- **M12 render encode path hardened** — encodes go through the new M12 render encode path.
- **M12 shader present path hardened** — and a defined M12 shader engine contract.
- **M12 game-local launch path** — defined; a bottle repair checklist and a native repair fallback were added.
- **Steam prefix init without wineboot** — speeds up launch on cold bottles.
- **DXMT winemetal migration staging** — verified.
- **Elden M12 shader corpus** — added for shader testing.
- **MTSP game rules + M12 fallback DLL checks** — updated.
- **Sharp artwork fallback** — uses fallback art for games missing images.
- **Phase 1–9 hardening** — diagnostic observability, bottle route contract hardening, M12 artifact verification, shader/PSO cache diagnostics, Metal binding descriptor hardening, command replay/barriers/visibility contract, runtime/migration perf cleanup, Mono/FNA/XNA reliability, release gates.
- **M12 cube pipeline CI check** — the standalone M12 cube runner is restored and hardens the M12 cube unix dylib staging; release CI runs it on every push.
- **Ad-hoc deep signing for DMG packaging** — the DMG build now signs deeply (Developer ID + notarization).
- **M12 dxmt surface isolated** — (#201) the updated M12 DXMT surface is isolated from M11.

### Fixed

- **RE4 DXMT diagnostics** — captured and staged.
- **mscompatdb disabled for M12 launches** — bypass wrapper load for M12; the wrapper is no longer needed because M12 uses the native DXMT surface.

## v0.46.5 — 2026-06-11

Steam secure launch args for protected games, FNA asset repair, GPTK prefix seeding, and D3DMetal offline launches.

### Added

- **GPTK repair, launch routing, and library UI hardening** — (#197) GPTK repair status is now surfaced in the library UI, with a clear launch-routing decision.
- **FNA runtime asset repair** — (#198) bottles with FNA games now repair their runtime assets (fnalibs bundle) on save.
- **FNA asset bundle repair (release)** — (#199) the release DMG verifier checks the fnalibs bundle.
- **GPTK prefix reseeding** — GPTK prefixes are now seeded with the D3DMetal DLLs and reseed themselves when the prefix is regenerated.
- **Party Animals steam secure args** — `PartyAnimals` (correctly appid 1260320) now launches with `-steam -secure`.
- **GTA V steam secure args** — `GTA5.exe` now launches with `-steam -secure`. Rockstar runtime prerequisites (C++ redist, scripthook) are preflighted.
- **BeamNG M11 secure rule** — `BeamNG.drive` routed through M11 with secure args.
- **Researched default launch args** — Epic and Ubisoft launcher bottles (and other researched titles) get default secure launch args.
- **Sharp Library launcher defaults** — the Sharp Library store launches games with sane default args; PR review state warnings are fixed.
- **D3DMetal offline launches** — D3DMetal Steam launches now default to running offline (no Steam client needed for single-player titles).
- **GPTK VC redist seeding** — GPTK prefixes now include the VC redistributable.
- **Mono FNA bottle save verification** — Mono FNA bottles are verified on save.

### Changed

- **Steam secure launch args** — deployed for `PartyAnimals`, `GTA5`, and `BeamNG.drive`; skipped on D3DMetal launches (which are offline).
- **Migration post-wineboot Steam updater dismissal** — the post-wineboot Steam updater is dismissed during migration so it doesn't block completion.


## v0.46.0 — 2026-06-11

Install hardening, backend lifecycle, Lucide icons, uninstall, and compatibility updates.

### Added

- **VC++ 2015-2022 runtime setup step** — new step in the setup wizard with x64 and x86 install cards. Downloads from Microsoft, runs via Wine `/install`, idempotent.
- **Kingdom Hearts HD 1.5+2.5 ReMIX** (appid 2552430) and **HD 2.8** (appid 2552440) — M11 pipeline rules, preferred EXE names, process patterns.
- **Uninstall MetalSharp** — Settings → Danger Zone button. Confirms, kills processes, `rm -rf ~/.metalsharp/`, shows success dialog, detached shell trashes `.app` from `/Applications`.
- **Lucide icons** — 26 inline SVGs replaced with tree-shaken `unplugin-icons` + `@iconify-json/lucide` components across 6 Vue files.
- **Dev-mode backend auto-restart** — `ensureRunning()` auto-restarts in dev mode so binary swaps work without manual restart.

### Changed

- **Backend lifecycle** — `killProcess()` sends SIGTERM then SIGKILL on quit. Production `ensureRunning()` no longer auto-restarts — only `start()` spawns. `cleanup()` is async and awaited in `before-quit`.
- **Goldberg emulator hardened** — install idempotency checks only core DLLs; `steam_interfaces.txt` regenerated if empty/incomplete; diagnostic logging on missing runtime.
- **Sharp Library Steam routing** — `SteamSetup.exe` routed to `steam::install_steam()` (correct `~/.metalsharp/prefix-steam/`); `Steam.exe` routed to `steam::launch_wine_steam()`.
- **GPTK prefix seeding** — macOS `ditto` replaces file-by-file copy for ~2GB/9000+ files; post-wineboot validation.
- **What's New modal** — fixed 640px width with compositing layer isolation and enlarged Close button tap target.

### Fixed

- **Dosdevice symlink guards** — snapshot/restore all dosdevice links around wineboot; post-check verifies `c:→drive_c` survived.
- **Wineboot migration window wait** — blocks completion until Steam self-update windows close.
- **External drive Z: drive** — `Z:\Volumes\...` fallback for external Steam libraries with idempotent `z:→/` dosdevice symlink.
- **Install wizard hardening** — Celeste `steam_api` fallback paths; Steam installer crash suppression.
- **Goldberg appid at toggle time** — `ensure_steam_emu_if_active()` repairs DLLs only; `ensure_real_steam_dlls()` deploys real Steam DLLs for normal launches.
- **Library grid row isolation** — bottle dropdown expands current row only.
- **CI test race** — unique migration preserve temp dir per thread.
- **CodeQL alerts** — resolve 3 `cpp/integer-multiplication-cast-to-long` in `CoreAudioBackend.cpp`.

## v0.45.5 — 2026-06-10

Migration prefix external drive support.

### Fixed

- **External Steam library migration** — migration handles Steam libraries on external drives by discovering their volume roots and creating correct dosdevice symlinks.
- **Wineboot on Steam prefix** — migration runs `wineboot -u` on the Steam prefix after update to register new Wine DLLs and registry entries.

## v0.45.0 — 2026-06-08

Post-update migration wizard, FNA Mono hardening.

### Added

- **Post-update migration wizard** — when the updater completes, MetalSharp re-launches into a migration view that preserves user data (settings, bottles, compatdata, Sharp Library apps) while installing the refreshed runtime.
- **VC++ and DirectX redistributable preflight** — runtime doctor checks and installs common redistributables.

### Fixed

- **FNA/Mono config templates** — ships Mono config templates in app bundle, guards against empty config files.
- **Setup wizard welcome page** — updated descriptions to match actual feature set.

## v0.44.0 — 2026-06-04

EAC toggle, kernel translation IPC bridge.

### Added

- **EAC offline toggle** — per-game `_winhttp.dll` deployment for offline EAC bypass with toggle UI in game cards.
- **Kernel translation IPC** — native macOS IPC bridge for Wine kernel32 translation layer.

## v0.40.0 — 2026-05-28

DXMT D3D12 support, M12 pipeline.

### Added

- **D3D12 to Metal via DXMT** — M12 pipeline for D3D12 games using DXMT's Metal backend.
- **Per-game shader and pipeline cache** — persistent cache dirs under `~/.metalsharp/shader-cache/` and `~/.metalsharp/pipeline-cache/`.

## v0.38.0 — 2026-05-24

Bottle manifest system, runtime doctor.

### Added

- **Runtime bottles** — bottle manifests under `~/.metalsharp/bottles/` with per-bottle prefixes, profiles, component state, and launch logs.
- **Runtime doctor** — per-bottle diagnostics for profile, compatibility, redistributables, and component repair.

## v0.35.0 — 2026-05-20

Linux packaging, anti-cheat evidence.

### Added

- **Linux DEB packages** — Debian package build and publish to GHCR via CI.
- **Anti-cheat evidence endpoints** — Steam anti-cheat evidence, module probe, and delta audit for protected launcher handoff analysis.

## v0.33.0 — 2026-05-19

Beta 7. Runtime bottles, installer profiles, migration wizard.

### Added

- **Installer bottle support** — Sharp Library classifies `.exe`/`.msi` installers, launches in bottle-aware Wine prefixes, scans for installed apps.
- **Migration wizard** — preserves user settings, Steam metadata, Sharp Library apps, and bottle settings across updates.
- **Runtime profile routing** — explicit bottle profiles for M9/M10/M11/M12/M32/Steam/Wine/installer flows.

## v0.24.0 — 2026-05-14

Auto-updater fix, Metal CI.

### Fixed

- **Auto-updater never ran install script** — `electron.remote` was undefined; replaced with `app:quit` IPC channel.

## v0.22.0 — 2026-05-14

Native C++ engine, DXVK MoltenVK for 32-bit D3D9.

### Added

- **Native C++ engine** — D3D11, D3D12, DXGI, XAudio2, XInput implementations via CMake.
- **DxvkMetal32 engine** — DXVK d3d9.dll through MoltenVK Vulkan→Metal for 32-bit games.
- **HTTP backend uses tiny_http** — replaced Actix.

## v0.18.0 — 2026-05-12

GPTK D3DMetal integration, MetalFX upscaling, 7 games confirmed.

### Added

- **GPTK D3DMetal for Steam DRM games** — `WINEDLLPATH` + `DYLD_FALLBACK_LIBRARY_PATH` for Apple's D3DMetal.
- **MetalFX 2x spatial upscaling** — half-resolution rendering with MetalFX upscaling to native.
- **DXMT config file** — `dxmt.conf` with MetalFX, 60fps cap, feature level 12_1.

## v0.17.0 — 2026-05-11

Beta 3. DXMT Metal-native D3D11, Wine 11.5 from source.

### Added

- **DXMT Metal-native D3D11** — renders through Metal directly, no GPTK or Vulkan needed.
- **MetalSharp Wine 11.5 from source** — built with 7 custom patches for DXMT integration.

## v0.6.0 — 2025-05-03

Windows Steam integration, DRM support.

### Added

- **Windows Steam** — MetalSharp Wine runs full Steam client with DRM support.
- **Uninstall button** — removes game files and appmanifest.

## v0.4.0 — 2025-04-13

Initial public release. 7 supported games, setup wizard, per-game auto-configuration, Electron UI.
