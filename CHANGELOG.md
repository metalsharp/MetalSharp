# Changelog

## Unreleased

### Changed

- **Ubisoft Connect header button** — after **Launch Ubisoft**, the header's primary button becomes **Stop Ubisoft** (with the Ubisoft icon) and the Steam start/stop action moves into the dropdown, instead of leaving Stop Ubisoft hidden in the dropdown. Stopping Ubisoft, or the status poll seeing Connect closed, restores **Start Steam** as the primary button.
- **Bundle tars come only from the `bundles` release** — stop tracking the stale `app/bundles/metalsharp-graphics-dll.tar.zst` (an M12-era build) and `app/bundles/fnalibs.tar.zst` (an older build than the release), which were force-added past the `app/bundles/*.tar.zst` ignore rule. Because CI only downloads a bundle when `app/bundles/<asset>` is missing, the tracked copies meant CI never tested the published assets. PR CI's D3D12 gate no longer copies the retired `dxmt-m12` lane.
- **D3DMetal Steam launcher handoff** — EVE Online's launch path (start Wine Steam with the D3DMetal environment, stage D3DMetal DLLs next to the game client, hand off through `steam://run/<appid>//` with Chromium GPU-off switches) is now a small table of launcher-gated games instead of EVE-only code. Assassin's Creed Odyssey (812140) now uses it too, replacing its Ubisoft Connect first-run crash-reporter retry monitor. Odyssey's running detection and Stop (which also stops Ubisoft Connect) are unchanged.

### Fixed

- **Red Dead Redemption 2 and the Rockstar Games Launcher on D3DMetal and VKD3D** — the launcher draws its UI with D3D10.1 and exited about 3 s after start because D3DMetal lacks `DXGID3D10CreateDevice`. The graphics bundle now ships WFDXCompat's launcher companion (`Graphics/dll/wfdxcompat/`), setup stages it to `~/.metalsharp/runtime/wfdxcompat/`, and the D3DMetal route sets `WFDXCOMPAT_RUNTIME_DIR` so MetalSharp's Wine loads it for `Launcher.exe` and `SocialClubHelper.exe`. RDR2 now starts the launcher directly (with Wine Steam running) instead of through a Steam handoff that lost the route environment; both routes run the launcher on D3DMetal, VKD3D passes `-api Vulkan` with its MoltenVK lane, and D3DMetal switches `system.xml` to `kSettingAPI_DX12` before each launch.
- **Ubisoft Connect client performance** — start Ubisoft Connect with the same Chromium switches EVE Online's launcher uses (`--disable-gpu`, `--in-process-gpu`, software WebGL through ANGLE), so its CEF UI no longer runs a GPU process under D3DMetal. Applies to Launch Ubisoft, the post-install launch, and the client start before a Ubisoft game.

## v0.76.0 - Unreleased

### Changed

- **License changed to AGPL-3.0-or-later** (from PolyForm Noncommercial 1.0.0).

### Added

- **Per-game executable selection** — choose and persist an installed Windows game's `.exe` from Steam/Ubisoft hero settings or the GOG/Epic game card. Choices are validated to stay within the game's install folder and are applied on subsequent launches.

## v0.75.0 - 09/28/2026

### Added

- **Ubisoft Connect library support** — launch/stop Ubisoft Connect in a dedicated D3DMetal-configured Wine prefix, map a writable external volume as Y: for game installs, discover locally installed games without account/API access, extract executable icons asynchronously into a local cache, and save/apply per-game graphics routes. Selecting a route stages its DLLs beside the chosen game executable; official Ubisoft page artwork is preferred for mapped titles, then SteamGridDB artwork is resolved with the saved Steam Art Manager API key if Ubisoft art is absent/unavailable, with local executable icons as the final fallback. Runtime migration preserves the Connect prefix and rebuilds Wine drive mappings. The user confirmed Far Cry 6 launches; other Ubisoft titles remain unverified.

### Fixed

- **Marvel Rivals launch lifecycle** — route Steam App ID 2767030 to D3DMetal by default through a Steam `-windowed` handoff, rediscover its detached game PID, and target its installed game processes for Stop and Cmd+Opt+Q.
- **Generic Wine process tracking** — recover game status from MetalSharp-managed non-Steam Wine executables when a launcher PID is lost, and let Stop/Cmd+Opt+Q kill those fallback processes.
- **Cyberpunk 2077 executable selection** — prefer `bin/x64/Cyberpunk2077.exe` for D3DMetal launches instead of relying on filesystem traversal that can select the RED launcher.
- **Ubisoft library/search lifecycle** — use the installed-game display name from Ubisoft's local uninstall registry, make search focus the matching hero game, keep Ubisoft Connect separate from Assassin's Creed Odyssey process detection, and retain Connect's tracked PID through its launcher-to-client handoff.
- **Ubisoft game process tracking** — rediscover live Ubisoft game PIDs from the exact executable command and install-directory working directory so Stop and Cmd+Opt+Q target the game, not the Connect client.
- **Ubisoft game launch selection** — ignore support/redistributable folders when selecting the game executable so launch and D3DMetal staging don't target bundled VC redistributers.
- **Steam Launch with D3DMetal Env** - Launch Steam with D3DMetal Environment so Steam can use it naturally without work-around hacks.
- **EVE Online** - Fix EVE Online Launcher loading and x64 client hand-off. 

## v0.74.0 — 2026-09-26

Launcher routing and artwork fixes, Sharp Library/setup polish, D3D9 route documentation, and the Age of Empires IV executable-selection fix.

### Added

- **D3D9 launch route** — documented the D3D9 option in the engine routes table and compatibility guide; added compatibility entries for Blasphemous 2, Balatro, and Assassin’s Creed Odyssey.
- **Assassin’s Creed Odyssey route** — launch it through Steam using D3DMetal.
- **Epic artwork fallback** — use TheGamesDB artwork when Epic metadata has no usable image.

### Fixed

- **Age of Empires IV executable selection** — Steam App ID `1466860` now prefers `RelicCardinal.exe` over competing executables such as `EssenceEditor.exe`. This selects the intended executable; it does not establish that the game successfully starts.
- **Epic graphics pipeline setup and launch** — repair Epic pipeline configuration and ensure launches use the selected route.
- **GOG Play and graphics routing** — restore Play behavior and selected-graphics-route handling.
- **Native macOS Steam launches** — route native macOS titles through the host instead of Wine.
- **Sharp Library executable routing** — select and launch the intended executable.
- **Helldivers 2 launch directory** — launch from the game install root.
- **Legacy updater recovery** — repair installations stuck in the older update/shutdown flow.

### Changed

- Polished Sharp Library and setup flows; refreshed the game compatibility page, engine-route descriptions, installation guidance, and README release links.
- Updated internal application, backend, and native project version metadata to `0.74.0`; included the release dependency batch.

## v0.73.0 — 2026-09-24

Restored library controls and VKD3D/D3D9 routes, external-library path resolution, and broad localization.

### Added

- **20-language UI localization** across the application.
- **D3D9 route** exposed in the renderer and runtime pipeline list.

### Fixed

- **Library controls and route inference** — restore missing library controls and infer the appropriate graphics route for games.
- **External Steam libraries** — resolve Wine drive paths for games installed outside the primary Steam library.
- **VKD3D route** — restore VKD3D runtime handling and its user-facing selector.

### Documentation

- Updated installation instructions and release-badge references.

## v0.72.0 — 2026-09-23

Release signing/notarization, Rosetta AVX support, VKD3D x87sidecar integration, and launch-architecture documentation.

### Added

- **Cooperative x87sidecar for VKD3D** — integrate the helper for compatibility-sensitive floating-point behavior.
- **Rosetta AVX advertisement** — advertise AVX capability to Wine games running under Rosetta; launch Isaac on the 32-bit DXMT route.
- **Compatibility entries** for Dredge and Hogwarts Legacy.
- **Star History** chart in the README.

### Changed

- License changed to PolyForm Noncommercial.
- Simplified runtime documentation to the five public launch routes and clarified D3DMetal modes, Goldberg behavior, dependency bundles, and VKD3D.

### Fixed

- Final DMG signing and notarization now run on the actual release artifact.
- Removed the VirusTotal release-scanning job and unnecessary Developer SDK validation from the DMG path.
- Made selected-member archive extraction safe, fixed zstd/hash-check error handling, and removed a stale Wine GSBASE hash check.

## v0.71.0 — 2026-09-20

Sunshine/Moonlight streaming, hardened migration verification, and setup-wizard recovery fixes.

### Added

- **Game streaming** — stream games to mobile devices through Sunshine and Moonlight, with bounded blocking operations, status re-arbitration, listener cleanup, and permission/process handling.
- **Palworld** compatibility entry.

### Fixed

- **Setup wizard completion** — restore the final “Launch MetalSharp” reference, correct the invalid page index, and remove the obsolete VC++ setup step.
- **VC++ migration/setup** — make installation asynchronous, make setup tests repeatable, recover from orphan installers, reject out-of-range PIDs and unsafe names, and handle `EPERM` as an alive process.
- **Migration verification** — pin the DXMT bridge signature/CDHash, use strict signature verification, and remove content verification that rejected valid preserved data. Existing Steam/GOG prefixes no longer receive the migration wineboot pass.
- **Streaming controls** — fix the Stream button’s drag-region hit target and restore the absolute curl path.
- **Window/theme behavior** — disable High Resolution (Retina) by default, reset corrupt saved window geometry, and correct light-theme chrome.

### Documentation

- Refreshed supported-game entries and README installation/quick-start images and instructions; clarified the VKD3D route.
- Batched dependency updates for `marked`, Biome, `@types/node`, `unplugin-icons`, and Vite.

## v0.70.0 — 2026-09-18

Major library redesign and runtime-routing cleanup, with a first-launch experience, Steam Art Manager, Wine/OpenGL work, setup improvements, and release hardening. The version was deliberately advanced from `0.61.0` to `0.70.0` to mark the major product update and complete visual overhaul—not because releases were omitted.

### Added

- **Library redesign** — themed top bar, collections/actions, settings overlay, refreshed logs and Sharp Library surfaces, themed header menus, and improved draggable window regions.
- **Steam Art Manager** — bundle the companion tool, add its launcher/control, remember the Steam path and API keys, provide first-use guidance, automatically refresh library artwork when Steam grid art changes, and let custom hero artwork override CDN art.
- **Quit guidance** — slide in a launch overlay reminding players to use Cmd+Opt+Q; leave Steam running when the shortcut force-quits games.
- **Startup and updater UX** — add the first-launch video, updater download progress and periodic release checks, and Markdown-formatted release notes.
- **Steam setup and runtime support** — show installation stages, prepare Wine wrappers before launch, make preflight actionable/best-effort, and bundle required archive/icon/zstd tools.
- **WineMetalGL OpenGL lane** — port the host OpenGL driver to Wine 11.17, record its host-driver hash, and select the appropriate `ntdll` variant for each Wine launch.
- **Unified gated ntdll runtime** — promote the unified runtime and ship the Wine GSBASE build for Unity/DXMT; update the pinned runtime hash for the Overwatch-safe rebuild.
- **Game compatibility and launch routing** — add/default routes for Witcher 3, Master Duel, Team Fortress 2, Ori, Nidhogg 2, and selected D3DMetal titles; select the CS2 executable correctly.
- **Steam route persistence** — create bottle manifests when saving routes and provide a 32-bit DXMT native-host bridge.
- **Wine Mono 11.3.0** bundled by default, with licensing documented.
- **macOS 14 support work** — target runtime components appropriately and harden the DXC container shim.
- **Installation** — clear Gatekeeper quarantine during install steps, use bundled tools without requiring Homebrew, and improve Steam installation setup.

### Changed

- Consolidated graphics route names and behavior: retired M12 runtime checks, folded DXVK fallback into VKD3D, rebuilt the DXVK lane for x86_64, and retired its i386 lane; preserve the selected `ntdll` variant and route-specific graphics environment.
- Retired outdated documentation/roadmaps and rewrote the install, route, Wine, D3DMetal, and Vulkan guidance; added a Steam repair guide/script.
- Refreshed the brand icon, default Electron window size, Retina behavior, Steam display scaling, and draggable header controls.
- Updated release and dependency workflows; dependency updates included Electron 44, `marked`, Iconify/Lucide, Biome, and the Vue Vite plugin.

### Fixed

- **Steam launch/runtime** — keep Wine Steam startup responsive; repair Steam installation-button behavior; restore Steam wrapper functionality; keep setup and launch controls responsive.
- **Backend routing and games** — reconcile Steam graphics routes with D3DMetal launches, preserve the correct route when switching, and ensure the Helldivers 2/CS2 launch paths are correct.
- **Updater/release** — publish progress atomically, derive DMG version from package metadata, report backend-version mismatches, narrow verification extraction, synchronize graphics/ICD checksums, and accept the DXMT manifest emitted by setup.
- **Security/dependencies** — address `js-yaml`, `tar`, `nanoid`, `fast-uri`, and `xmldom` advisories; authenticate VirusTotal uploads.
- **Setup and UI** — fix Steam installation preflight and separate library drag regions from interactive header controls.
- **Uninstall** — remove MetalSharp-owned data safely without deleting unrelated user data.

## v0.61.0 — 2026-08-28

Hand-written C backend becomes the active runtime; adds third-party launchers, GameJolt, emulators, and signed-runtime migration/release work.

### Added

- **Hand-written C backend** — make it the active runtime reference, implement launch/runtime and Steam-sync behavior, and add backend parity comparison/normalization tests and crash reports. Remove the Rust backend after parity work; remove obsolete “basic” file suffixes and local absolute rule paths.
- **Launcher integrations** — add GameJolt and isolated Epic, Ubisoft, Rockstar, EA App, and Battle.net launcher/library support; add a native Epic library. Battle.net was subsequently removed as a supported launcher and isolated on Wine Staging while that integration was evaluated.
- **Emulator libraries** — add managed PCSX2, RPCS3, and SharpEmu environments; add experimental shadPS4 library/capability manifests and safe update handling; add emulator library sidebars and setup actions. Remove the dormant RPCS4 provider, simplify emulator actions, and accept PS2 images.
- **FEX update channel** for runtime updates.
- **Theme polish** for Beach and Forest themes; official PS3 firmware link/button and firmware onboarding for RPCS3.
- **PolyForm Noncommercial licensing** and community health files.

### Changed

- Keep the C backend alive for long operations; keep it alive during the update handoff while managed Wine processes are stopped; make migration and ordinary relaunch paths work with the new backend.
- Preserve GOG installation state when stopping and correctly resolve installed GOG games for launch. Keep the dedicated Sharp Library launcher setup out of the general setup flow.
- Add native tool paths and bundle/runtime hash refreshes; document Homebrew-tap installation and compatibility testing.
- Refresh release bundles from the published manifest, pin signed-runtime hashes, split and scan release DMGs with VirusTotal, and update CI for Rosetta/native C-backend and PCSX2 archive/capability tests.
- Batch Electron, Biome, Node type, and Rust `libc` dependency updates.

### Fixed

- **Steam library** — refresh library contents and executable routing, preserve external-library/game sync, fix TF2 executable selection and Steam setup, and align C migration with Rust preservation behavior.
- **GOG and launchers** — preserve GOG install state after Stop; resolve installed GOG games; repair GOG auth artwork and Mono persistence; make launcher environments isolated.
- **Emulators** — make PCSX2 capability and PS2 image indexing exact; complete RPCS3 folder onboarding; avoid redundant shadPS4 manifest writes and clean interrupted updates safely; bind imports to detected runtime capabilities.
- **Migration/runtime** — match C migration behavior to existing data preservation, normalize quarantined assets, use fixed macOS tool paths for bundle staging, and keep signed-runtime verification aligned with release bundles.

## v0.60.0 — 2026-08-17

Standalone VKD3D-Proton lane, global game controls, route-aware library refresh, and the glass-header/theme experience.

### Added

- **Independent VKD3D-Proton lane** for D3D12 through Vulkan/MoltenVK, with pinned runtime payloads staged during install/migration and protected from DXMT route contamination.
- **VKD3D route support** — deploy DXVK components for D3D9/10/11, configure MoltenVK and DXVK runtime options, add diagnostics and `dxvk.conf`, and route games that resolve to M12/VKD3D through the lane. Keep an explicit DXMT rollback and seed DXMT configuration/markers for DXMT bottles.
- **Global Cmd+Opt+Q** to force-quit running games while leaving Steam alive.
- **Sidebar controls** for MetalFX scale (`1.75`, `1.50`, or off), Msync, and controller input; bundle the XInput controller shim DLLs.
- **Library lifecycle refresh** — refresh library state automatically and return a stopped game’s card to Play, including when a game exits externally; discover installed games in shared external Steam libraries and refresh after installs.
- **Glass migration wizard** and redesigned setup wizard; refresh the library/logs UI, theme picker, and game cards.
- **Themes** — add Banana, Lava, Beach, and Xray; make the picker scrollable and remove the developer theme from normal selection.
- **Noncommercial license and runtime installer**, plus community health files.

### Changed

- Move the HTTP backend from `tiny_http` to multi-threaded Axum with a blocking pool.
- Replace M9/M10/M11/M12 labels with the public graphics route names; retire M9 and remove M12 from user-facing selectors.
- Keep VKD3D’s Vulkan/MoltenVK surface in its own runtime lane and never deploy it into DXMT `system32`; avoid re-running the VKD3D installer during bottle saves.
- Update compatibility rules and pipeline labels, including the Sons of the Forest DXMT route and the 9 Kings VKD3D diagnostic override.

### Fixed

- **Process control** — restore force-kill ownership checks for Wine argv, parse Windows executable arguments correctly, register the `/game/running` IPC route, poll registered PIDs, and keep library cards in sync with game exits.
- **Steam library** — show the library before slow scans, make `/scan` single-flight, detect new installs promptly, make Refresh perform a full scan/reload, sync external-library games, hide Wine Steam desktop shortcuts, and stage the Steam runtime for D3DMetal bottles.
- **Protected launches** — prepare D3DMetal protected games without blocking non-D3D12 titles on Agility SDK setup.
- **Pipeline transitions** — quarantine stale VKD3D `d3d12core.dll` when switching to DXMT and self-heal `dxmt.conf`.
- **Security and dependencies** — patch the `undici` Dependabot alerts and tighten release/DMG bundle handling.

## v0.59.9 — 2026-08-15

### Fixed

- Pin the DXMT Metal shader version to Metal 3.1 (`310`) for the shipped graphics runtime.
- Correct the README release badge.

## v0.59.7 — 2026-08-15

### Fixed

- Rebuild the DXMT graphics DLLs for the macOS 14 / Metal 3 minimum deployment target.
- Clarify public graphics-route descriptions in the README.

## v0.59.5 — 2026-08-13

Major security/stability hardening across the local API, process control, Wine compatibility shims, DXMT/VKD3D routes, installer/updater, and release packaging.

### Added

- **VKD3D-Proton public lane** — rename M12 to VKD3D, retire the separate M9 route, unify M10/M11 as DXMT and their 32-bit variants as DXMT (32-bit), and use DXVK-MacOS for D3D9/10/11 on the Vulkan route. Keep DXMT and VKD3D payloads separate; pin MoltenVK/DXVK assets, configure Vulkan, deploy the expected DLLs, and add route/bundle checks.
- **GPTK 4 beta 2 optional overlay** with allowlisted repair endpoint and conditional download; retarget the optional overlay to GPTK 3.0 and refresh its state after D3DMetal settings changes.
- **EAC support substrate** — package the Linux EAC substrate, stage it across installs/migrations, add opt-in game-card controls and protected-game rules, validate assets on clean builds, and verify protected launcher files remain untouched.
- **Force-quit and stuck-game recovery** — add backend process-tree ownership, detach/reap Wine helper processes, and allow the UI to evacuate hung games safely.
- **FNA/Mono and native host compatibility** — add a managed-executable bridge and harden Win32, loader, networking, audio, and threading behavior (details in Fixed).

### Changed

- Remove Developer SDK packaging and requirements from DMG/release manifests and stop verifying the retired DXMT-VKD3D SDK lane.
- Use a version-aware WineMetal shader configuration and rebuild DXMT for the supported macOS/Metal baseline.
- Update setup/migration screens to glass styling and redesign setup with the application logo, Rethink Sans, glass, and foil styling.
- Batch Electron, Vue, Vite, Iconify, Biome, Rust networking/database, and support-library dependency updates; restore formatting gates in CI.

### Fixed

- **Local API and process security** — authenticate local backend requests; bound request bodies; validate Steam AppIDs; constrain process termination to MetalSharp-owned executable/process trees; fix game-process argv ownership; reject unsafe GOG auth flags; prevent local path traversal in Sharp Library artwork/cover writes; reject symlinked cover directories; guard uninstall targets; safely handle privileged updater paths and DMG ejection; verify downloaded installer artifacts.
- **Updater/install/release** — compare prerelease versions correctly, publish DMG versions from package metadata, surface backend-version mismatch as an error, clean failed Steam staging prefixes, never delete a Steam prefix before reinstall succeeds, inject the EAC substrate into the scripts-tools bundle, verify pinned bundle checksums, and harden package preparation.
- **Win32/kernel32/loader** — back SRW locks with mapped pthread state; fix `LoadLibrary` resolution, `GetModuleFileNameA`, `RtlLookupFunctionEntry` null-image handling, environment-variable shims, UTF-16 entry points/string termination, PE relocation/import/export/resource bounds, module find-data filenames, thread exit/completion reporting, named-pipe directory/handle behavior, and per-call pipe handles.
- **Networking/audio** — implement `WSAWaitForMultipleEvents` over real events, honor `TIMEVAL` in `select`, bound `GetAdaptersAddresses`, guard DirectSound buffer lifetime, synchronize CoreAudio callback state, bound stereo render output, clamp X3DAudio matrix stride, and release Mach send rights on every error path.
- **FNA/Mono and launch** — handle `LoadLibrary` failures and UTF-16 names; preserve borrowed handles in `CloseHandle`; pass managed executable parameters through the mscoree bridge; constrain FNA game directories; stop returning the game directory when executable resolution fails; scope Process Manager shutdown; and restrict teardown to owned process trees.
- **API and settings** — atomically persist settings and synchronize state; remove duplicate Rust-bridge listener lookups; isolate compatibility database fixtures; add tests for the security and runtime fixes.

## v0.59.1 — 2026-08-08

Stabilizes the new Mono/FNA and VKD3D launch routes, improves game-stop controls, and adds consented developer diagnostics.

### Added

- **Cmd+Q game stop** and UI launch controls; center sidebar launch controls and update registered-process polling so cards return to Play on game exit.
- **PostHog developer diagnostics** for consented diagnostic reporting.
- **Reminiscence** M11 compatibility rule; **Stardew Valley** routes through the FNA/Mono path.

### Fixed

- **Mono/FNA** — restore Steamworks before FNA handoff, stage Terraria launcher by executable name, support Terraria WinForms/ReLogic.Native shims and Stardew Steamworks.NET, route .NET Core games correctly, repair wrapper/Mono/.NET 6/GDI+ and Unity launch regressions, and guard runtime directory and DLL staging.
- **VKD3D/MoltenVK** — synchronize VKMT/MoltenVK on install; resolve its lane before Wine’s own libraries; use the validated VKMT stack to boot Control; deploy DXVK `dxgi` + `d3d11` for D3D11-on-VKD3D; direct-launch with real Steam files; refresh caches/diagnostics; isolate `system32` deployment and freeze the bottle route during save.
- **DXMT** — select macOS-appropriate Metal shader versions.
- **Pipeline state** — isolate graphics DLLs when switching routes and mutate runtime pipeline rules consistently with bottle state.
- Preserve the saved theme on startup and fix Steam launch/bottle-spinner races.
- Update `js-yaml` and resolve Rust/clang formatting CI failures.

## v0.59.0 — 2026-08-06

Introduces versioned Mono/FNA profiles, launch routing and payload deployment; adds controller and graphics controls.

### Added

- **Mono profile discovery and routing** for Unity Mono, FNA, XNA, MonoKickstart, and IL2CPP. Detect baseline versus modern Mono requirements, deploy version-matched prebuilt runtime payloads when saving a bottle, and dispatch kickstart launches without compiling at launch time.
- Bundle version-matched Unity Mono, XNA, SDL3, and related runtime payloads; add readiness checks and route-specific configs.
- **Controller input** selector for XInput/DInput shims and ship the corresponding DLLs in `lib/metalsharp`.
- **Sidebar MetalFX and Msync toggles**, including MetalFX scale selection and passthrough of the selected factor.
- Move the D3D12/M12 stack to VKD3D-Proton (D3D12 → Vulkan → MoltenVK), with DXMT rollback available.

### Fixed

- Isolate MetalFX/MSync settings and stop environment overrides from leaking into the wrong launches.
- Repair bottle-save races, stale component state, migration config survival, M12 install reconciliation, and failed Wine-prefix migration cleanup.
- Isolate MetalSharp’s Wine runtime from foreign launchers such as CrossOver/SakuraGiri, disable verbose Wine tracing by default, and preserve legacy OpenGL compatibility contexts.
- Fix the mapped-address virtual-memory test and add the Mono-route discovery roadmap/changelog and bundle manifest updates.
- Batch Dependabot dependency updates.

## v0.58.0 — 2026-08-04

### Added

- Banana, Lava, Beach, and Xray theme packs; make the theme picker scrollable and remove the developer theme from user-facing selection.
- Noncommercial license and a runtime installer.
- Community health files and support guidance.

### Fixed

- Patch `undici` security advisories across affected dependency lines.
- Clarify README graphics-route names, 32-bit D3D10/D3D11 coverage, Apple Silicon/ARM64 build details, Homebrew installation, and release/download links.

## v0.57.0 — 2026-07-27

### Added

- New library column layout, theme-aware cards and photo textures; ship theme packs, a theme picker, and developer library-preview mode.
- Document Homebrew-tap installation.

### Fixed

- Clean migration state before relaunch.
- Correct the Jedi: Fallen Order title and refresh supported-game entries.
- Update Electron, Electron Builder, Vite, Vue plugin, Biome, and Rust `libc` dependencies.

## v0.56.8 — 2026-07-22

### Changed

- Refresh the library and Logs interfaces.
- Update the vulnerable `fast-uri` dependency to 3.1.4 and refresh README information.

## v0.56.5 — 2026-07-21

### Added

- **MoonScraper installer support** — detect Inno Setup installers, extract their payload natively, install the discovered app into the right Sharp Library bottle, and document the fallback.

### Fixed

- Automatically synchronize installer bottles and support WineBare installers.
- Tighten Java/WebView detection so scan noise from large files does not cause false positives.
- Remove an installer bottle when uninstalling its app and use standard fallback artwork.
- Refresh app screenshots and description.

## v0.56.3 — 2026-07-21

### Added

- **OpenGL 2–4 bridge** — port the OpenGL bridge from PR #307 and complete its CMake/cherry-pick integration.
- Port the library UI and visual polish; add SharpView M12 dry-run/timeouts and Settings restart UX.

### Fixed

- Repair Homebrew installation in the setup wizard and parse `mtsp-rules.toml` using the TOML 1.x document API.
- Enforce i386 DXMT installer lanes, gate the Developer SDK, and stop rebuilding M12 in the installer CI path.
- Patch the `brace-expansion` ReDoS advisory and synchronize dependency locks; update Prettier, Electron, Vue, Node types, Iconify, TOML/Rust crates, and GitHub Actions.
- Exclude third-party submodules and SDK caches from clang-format checks; fix Rust map iteration lint.

## v0.56.1 — 2026-07-17

### Fixed

- Restrict release migration to the prefix-only operation and preserve the expected runtime/migration contract.
- Repair clean M12 runtime setup and cover all Agility SDK cache surfaces with tests.
- Keep the bottle workspace inside its game card.

### Changed

- Refine sidebar glass/translucency and active-navigation styling, synchronized with the selected app theme.

## v0.56.0 — 2026-07-17

### Added

- **M12/DXMT runtime separation** — isolate the M12 DXMT lane, freeze validated DXMT production surfaces, enforce bundle/deployment and direct-launch contracts, and add conformance gates.
- Gate Agility SDK setup on game requirements; update the default M12 rule to the proven launch shape and repair Agility installation/routing.
- Add a glass migration window and simplify bottle runtime-status UI.

### Fixed

- Verify M12 bottle saves, protected launches, and runtime receipts; refresh preserved bottles during migration.
- Restore sidebar glass outside low-performance mode and keep loading spinners active in low-performance mode.
- Clarify migration phase UI; remove redundant DMG-clean setup checks and Wine probes from release gates.
- Update release tooling/dependencies, including artifact actions, electron-builder, Vite, Iconify, Biome, and CodeQL actions.

## v0.55.1 — 2026-07-15

### Added

- Make the Developer SDK an opt-in release artifact and detach DMG release creation from it.

### Fixed

- Harden post-update migration handoff and bottle operations/backend recovery.
- Use native TLS for backend downloads.
- Keep long-running GOG backend requests alive.
- Synchronize application, backend, and native version metadata.

## v0.55.0 — 2026-07-15

Introduces the standalone C backend/release path and its contract tests, architecture checks, and release-bundle pipeline.

### Added

- **Standalone C backend** — create the C runtime and tests, ship it through the pinned Rust-to-C compiler, capture a standalone link manifest, force a portable SHA-2 backend, and validate Electron calls against C routes.
- **Backend contract suite** and test-count enforcement; validate the C backend in DMG workflows and keep production DMGs on the C-only backend path.
- **Release pipeline restoration/hardening** — restore bundle builds, refresh stale locked release bundles, validate C-only bundle setup, verify the DMG and release inputs, and make backend updater port handoff reliable.
- Make macOS target architectures explicit and keep Wine test targets x86_64.
- Restore CodeQL C++/TypeScript analysis and add release/build job-control contracts.

### Fixed

- Fix the backend contract for `0.55`, Homebrew setup detection, Terminal launch, Electron backend import order, C-only installer bundle resolution, DXMT MinGW GUID formatting, and release-bundle refresh.
- Address Rust clippy warnings on Rust 1.97; remove obsolete Rust CodeQL analysis.

### Changed

- Clarify C backend runtime ownership and use C backend artifacts consistently.
- Update README and batch Electron, Vue, Iconify, Biome, Node types, and GitHub Actions dependencies.
- Add and then revert a post-Rosetta exit-strategy roadmap pending further evaluation.

## v0.54.5 — 2026-07-09

Testing-surface hardening, pre-commit strictness, and compatibility database refresh.

### Added

- **`tools/ci/validate-rules-toml.py`** — lightweight Python validator for `configs/mtsp-rules.toml`. Catches: TOML parse errors, duplicate `[overrides.APPID]` sections, missing/empty `name`, missing/unknown `pipeline`, and unrecognized sub-table keys.
- **`tools/ci/check-doc-freshness.py`** — warns on docs without an `Updated:` header or older than 120 days; also verifies `CHANGELOG.md` has a section for the current version. Warn-only by default; `--strict` makes it an error.
- **`.github/hooks/pre-commit`** + README — opt-in shared pre-commit hook. Runs `clang-format --dry-run --Werror`, `tsc --noEmit`, `biome ci`, `prettier --check`, and the rules-TOML/doc-freshness validators locally when relevant files are staged.
- **609 default rule entries** for Steam AppIDs — bulk-add of game compatibility rules for DX9/10/11/12 pipelines, sourced from Steam store data and community testing. Pipeline distribution: m9: 54, m10: 2, m11: 471, m11_32: 70, m12: 12.

### Changed

- **Pre-commit policy: fail-hard on missing toolchains.** The hook fails when required formatting, Node, or Python tooling is absent instead of silently skipping checks.
- **Compatibility database** — `docs/compatibility/GAMES-SUPPORTED.md` now documents the M9, M10, M11, M11-32, M12, and D3DMetal pipeline coverage and corrects the Party Animals AppID (1260320; the prior 1823720 was incorrect).

### Fixed

- **Party Animals AppID correction** — was 1823720 (Mail Mole); correct ID is 1260320.

### Documentation

- Added `**Updated:** 2026-07-08` headers to 37 docs that lacked a date stamp.
- Archived dead/historical roadmaps to `docs/archive/roadmaps/`. `docs/archive/README.md` documents the archive policy.
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
- **Dependency bumps** — bumps `vue`, `lucide-icons`, `biome`, and `electron`. Patches vulnerable npm transitive deps.
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
