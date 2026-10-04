# GPUI Port Feature and Acceptance Matrix
**Status:** Visual preview validated; native production replacement and Electron retirement remain pending.
**Updated:** 2026-10-03

**PR boundary:** the user directed that GPUI replacement and Electron app removal land in the same PR (#703). Keep it draft until all capabilities, data/backend integration and local acceptance pass. Electron is deleted only as the last migration change; it remains intact throughout port development. Preserve support for games implemented using Electron technology; that is unrelated to the desktop framework being retired.

This matrix is the tracked parity checklist for the [GPUI port roadmap](gpui-port-roadmap.md). `Not started` means no GPUI parity implementation; it does not mean the current Electron feature is absent. Do not mark a row complete based only on compilation: record automated/manual evidence in the verification column.

## Local validation-machine baseline

| Item | Baseline / action |
|---|---|
| Machine / architecture | Apple Silicon validation host; arm64 (`uname -m` / Darwin kernel identifies Apple Silicon). |
| macOS | `sw_vers` reported macOS 27.2, build `26B5091g` on 2026-10-03. Recheck before acceptance because OS updates may change it. |
| Xcode toolchain | `xcode-select -p` reports `/Library/Developer/CommandLineTools`; no `/Applications/Xcode*.app` was present. A stale helper in `~/.local/bin/metal` hardcodes a removed Xcode beta path. GPUI's default offline Metal-shader compile therefore fails in this shell; the Phase 1 spike currently enables GPUI runtime shaders and must verify runtime behavior. Confirm a supported Xcode/Metal toolchain before any release build. |
| Checkout safety | The previous checkout was preserved separately before creating the port branch; keep it intact during migration. |
| Test account / title list | To be confirmed by Avery before exercising sign-in, installation, or game launch. Use only user-approved test titles/accounts. |
| Backup and rollback | Back up current `/Applications/MetalSharp.app` and user state before any install or migration test. Keep the candidate side-by-side and isolated; verify Electron can still open and launch a known-good title after GPUI testing. No production app replacement until signed acceptance. |

## Existing UI and user journeys

| Area / source | Current behavior to preserve | GPUI status | Verification evidence |
|---|---|---|---|
| App shell — `app/src/renderer/App.vue` | Navigation, startup state, Steam/runtime status, update/install polling, setup/migration presentation, startup video, overlays, notifications, preview modes. | Not started | — |
| Setup — `SetupWizard.vue` | First-run sequence: Welcome → install runtime/support assets → install Wine Steam → final page for device name + optional Steam Web API key + optional TheGamesDB artwork-fallback key → launch. Runtime/Steam progress, retry/error, and reopen-for-repair must work. A no-Steam test machine can validate setup UI only in deterministic mock mode; do not attempt real installation for UI verification. | Approved synthetic preview preserved. Opt-in isolated connected setup binds the three-page layout to dependency readiness, real runtime/Steam progress, masked keys and guarded completion; no sample-library fallback. Migration is gated, post-setup final-library integration and live install/retry/repair acceptance remain pending | — |
| Migration — `MigrationView.vue` | Migration readiness, start/progress polling, completion/error and app restart handoff. | Not started | — |
| Steam/Ubisoft library — `LibraryView.vue`, `GameCard.vue`, `GameLaunchSettingsPopover.vue` | After setup, backend loads Steam ownership and installed manifests plus Ubisoft library; refreshes automatically on install/uninstall/download via watcher/poll/focus fallback. Preserve artwork-led Play and Collection pages, search/filter, install/running/download states, launch/configuration/actions, Steam and Ubisoft controls. A machine with no Steam client/account cannot validate live sync; use representative deterministic mock catalog for UI and a real backend route for integration separately. | Approved bundled Play/Collection preview preserved. Opt-in connected workbench loads real Steam/Ubisoft library payloads and sends launcher/game start-stop routes; final artwork-led library integration and real-game acceptance pending | — |
| Sharp Library — `SharpView.vue` | GameJolt/GOG/Epic/SharpEmu and emulator catalog flows; install/update/launch/stop/uninstall; configuration, process/update polling and per-provider errors. | All eight source previews implemented; advanced dialogs and live integrations pending. | User appearance approval; [preview gaps](../../app-gpui/PAGE-PARITY.md). |
| Settings — `SettingsView.vue`, `SettingsOverlay.vue` | Persisted config, runtime and storefront controls, re-run setup, paths, API keys, data repair, app/update actions. | Approved eight-card preview remains synthetic/non-editable. Connected workbench persists typed runtime preferences through partial `/config` writes and masks live key inputs; final overlay integration and other settings workflows remain pending. | User appearance approval; [Settings validation](../../app-gpui/SETTINGS-PARITY.md). |
| Logs — `LogsView.vue` | Read/filter/refresh runtime/backend diagnostics; empty/loading/error states. | Approved synthetic drawers remain unchanged. Connected workbench reads bounded/redacted backend logs on demand; final drawer integration, filtering/export and crash-report workflows remain pending. | Unit tests, user appearance approval; [preview gaps](../../app-gpui/PAGE-PARITY.md). |
| Streaming — `StreamingOverlay.vue` | Sunshine status/start/stop, setup/credential/error guidance, Moonlight connection help. | Safe simulated streaming panel implemented; real services/pairing pending. | Preview-only controls; [preview README](../../app-gpui/README.md). |
| Process manager — `ProcessManagerOverlay.vue` | Process sampling, actions, overlay window, global shortcuts, force-quit semantics. | Not started | — |
| Shared design/accessibility | Game-focused, artwork-led setup and library; compact app navigation; seven themes (`dark`, `light`, `skeleton`, `forest`, `orange-peel`, `dragonfruit`, `lava`); 20 locales; custom font/icon/artwork treatment; markdown; keyboard navigation/focus, screen-reader/accessibility labels, high-DPI. | Not started | — |

## Electron capabilities to replace

Inventory baseline: `app/src/main/preload.ts` declares the renderer-facing API; `app/src/main/index.ts` registers main-process IPC handlers. Keep tests for each permission-sensitive service and update statuses as native replacements land.

| Capability group | Existing operations (representative; reconcile against preload/IPC source) | GPUI replacement / status | Verification |
|---|---|---|---|
| Backend ownership | Start/ready/retry/health/request/restart/stop/PID/base URL (`BackendBridge`, `UpdaterBridge`). | Opt-in connected host/client implemented with owned-child PID/home readiness, occupied-port refusal, bounded HTTP timeouts, no proxy/redirects, length-delimited JSON and non-retried mutations. GPUI-owned sessions require per-launch header authentication; legacy behavior remains unchanged. Native connected SIGTERM reaped its owned backend in isolated acceptance. Explicit restart is implemented; automatic crash recovery and production acceptance remain pending. | Validate clean startup, ready timeout, busy port, owned-child cleanup, and no adoption/kill of foreign process. |
| File selection / reveal | Folder, executable, image, emulator BIOS/package/game, SharpEmu root pickers; Finder path reveal; open owned emulator resources. | Native folder picker in preview and native EXE picker/persisted Steam/Ubisoft executable route in connected workbench implemented. Other dialogs/path allow-lists remain pending. | Manual picker cancel/select; typed filters, path ownership and invalid-path tests. |
| OAuth / web content | GOG and Epic flows in Electron BrowserWindows/webview, callback extraction and cancellation. Settings links and web content sometimes use Electron-managed browser windows. | Native macOS `WKWebView`/`NSWindow` mini-browser module now exists with a read-only live URL header, navigation controls, purpose-scoped HTTPS rules, ephemeral GOG/Epic/help stores, and a named persistent GameJolt store. Connected-mode GOG initialization/auth-code handoff and Steam/TheGamesDB help actions are wired; default preview stays offline. Offline native chrome/read-only/min-size/reload/close checks passed; anonymous Steam Store loaded/redirected with a visible current-URL header, and the user approved its look. No account login acceptance is claimed. Epic support/auth/sync routes and a user-approved fixed read-only extractor at the exact JSON redirect endpoint are wired, with native URL/document/generation checks and bounded/ambiguity-tested parsing. End-to-end account acceptance remains pending. | Unit policy tests and offline macOS build pass. Still require provider allow-list/redirect review, GOG callback/session tests, Epic live result/state acceptance, GameJolt data cleanup, cancel/failure tests, secret-redaction review, and full multi-window/history/permission/lifetime acceptance beyond the initial local chrome checks. |
| External system integration | Open vetted URLs, copy text, clipboard, Steam Art Manager, DMG eject, Homebrew/dependency setup, Steam fix. | Not started | Allow-list and argument tests; manual validation on machine. |
| Watchers / notifications | Steam `steamapps` changes, grid-art changes, GameJolt progress, launch overlay. | Not started | Watcher debounce/recovery/unsubscribe and notification checks. |
| Lifecycle / global controls | App activate/reopen, app quit/uninstall, process-manager/force-quit shortcuts, main-window focus/blur. | Not started | Multi-window, shortcut conflict, launch/quit/reopen and safe child-process tests. |
| Updater / install / migration | External updater process handoff, status/clear state, DMG/install and migration restart. | Not started | Interrupted/failed/successful transaction tests, rollback, signing and local app install. |

## Backend route family checklist

The GPUI client should preserve API semantics and call the C backend instead of recreating domain logic. Inventory every actual route used by the renderer with source searches and tests; the list below groups the currently documented API surface and must be expanded into route-level parity tests before replacing Electron.

- Health/configuration: `/status`, `/config`, setup/device-name/state/dependencies/install/progress, update and migration.
- Steam/Ubisoft: library, install/status, launch/launch-game/stop, artwork/API-key sync, route/graphics configuration, process/running status, Steam compatibility/store artwork paths.
- Bottles/graphics: bottles list/doctor/prepare, runtime readiness, route information, cache/diagnostics and launch controls.
- Sharp Library/providers: library management and import, GameJolt/GOG/Epic/SharpEmu auth/library/install/update/process/launch/stop/uninstall routes.
- Emulators: RPCS3, PCSX2 and shadPS4 status/setup/game/firmware/update/launch/stop/path/compatibility routes.
- System operations: logs, process sampling/actions, streaming/Sunshine, backend health/restart, app update and data repair.

**Phase 0 exhaustive-route task:** [source-derived discovery inventory](gpui-backend-route-inventory.json) now lists 258 exact C-router method/path pairs and 21 prefix/unmatched comparisons requiring manual expansion, plus literal renderer/GPUI candidate references. It does not claim exhaustive dynamic-call discovery or schema/parity verification. Finish annotating request bodies, response types, actual call sites and acceptance/test IDs; keep computed paths and backend-only routes as manual review items. CI checks the generated artifact for drift.

## Distribution, data, and release constraints

| Contract | Baseline to preserve | Verification / gate |
|---|---|---|
| User data | `~/.metalsharp` default production data, runtime, Steam prefix, provider/emulator data, saves/configs, logs/caches. GPUI preview uses only a dedicated test home and validation port. | Before/after manifests and representative read/launch; backup/restore; no setup or migration against production data without explicit test plan. |
| Ports | Normal backend `127.0.0.1:9274`; validation-only GPUI default `9276` (configurable for tests). | Assert preview never defaults to 9274 or an unapproved production home. |
| Runtime assets | Keep existing split runtime/graphics/assets/scripts/Steam bundles, roots, manifest hashes, licensing and installer contracts. Replace only app payload once full port is accepted. | Existing bundle verifiers remain green; new GPUI payload has independent hash/layout verifier. |
| App identity and URL scheme | Current `com.metalsharp.app`, `metalsharp://` and updater associations are owned by production Electron during qualification. | Candidate uses distinct local identity and does not claim production scheme/update state until approved. |
| CI and publication | Current Electron app remains the published production app. GPUI build/test may be added as non-publishing CI. | Release workflow cannot publish GPUI before local acceptance artifact (machine, commit, OS, test record, 14-day minimum soak, rollback proof) and explicit go/no-go approval. |

## Electron-removal gate (required in this same PR)

Electron desktop source/build/dependencies are **not** removable while any GPUI integration below is incomplete. Once replacement implementation and evidence are ready, retire Electron in the final commit(s) of draft PR #703, then complete clean-install, upgrade/rollback, signing, resource and target-machine acceptance before asking to merge. The matrix rows above, exhaustive `preload.ts` capability inventory, backend routes and packaging are the checklist; update each with test evidence rather than marking source similarity as parity.

- [ ] Native app lifecycle, first-launch/setup/recovery, windows, shortcuts, clipboard and notifications.
- [ ] Production C backend launch/health/request/restart/shutdown, packaging/resources, real user-home compatibility and owned-child safety.
- [ ] Every backend route called by renderer, exercised by typed route-contract tests and live isolated-home smoke tests.
- [ ] Complete library, settings, logs, streaming, Sharp/providers/emulators, process manager and migration workflows; no placeholder-only production path.
- [ ] Native pickers/reveal/open/system links/data repair with canonical root and URL checks.
- [ ] Integrate and accept the native MiniBrowser for Steam/TheGamesDB/help links and GOG/Epic/GameJolt; accept Epic result/state handling, finish GameJolt session cleanup, download/popup/permission policies, OAuth callback tests, and no-secret logs. Connected-mode GOG/key-help actions are wired and offline tests pass, but native-window/provider acceptance and full browser parity are outstanding.
- [ ] Watchers, artwork network helpers, launch overlays and process controls.
- [ ] Native updater/install/uninstall, protocol, icon/resources, signing/notarization and release rollback.
- [ ] Search proves no application-level Electron imports/APIs/dependencies/scripts/package artifacts remain; game-level Electron launcher support is retained.
- [ ] All relevant tests pass and target-machine acceptance is recorded before the PR is made ready.

The port remains early; no production parity rows are complete. Local acceptance is mandatory before merge/release.
