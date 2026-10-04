# GPUI Desktop Port Roadmap
**Status:** In progress — approved visual preview preserved; opt-in connected backend/setup/launcher/key/GOG integration started. Full production parity and acceptance are incomplete.
**Scope:** Full replacement of the Electron desktop app with a native GPUI app. The current app remains the production app until the new app passes the local-machine acceptance gate.
**Updated:** 2026-10-03

## Objective and non-goals

Replace MetalSharp's Electron/Vue desktop shell with a Rust/GPUI native application while preserving user-visible behavior, backend API behavior, runtime/data compatibility, and the existing C/C++/Objective-C graphics stack.

This is a full desktop-app port, not a rewrite of the graphics engine or C backend. The initial architecture should keep `app/src-c` as the backend process and preserve its HTTP API; keep the existing engine, runtime bundles, launch behavior, and user-data layout unless a concrete blocker requires a separately reviewed change. The Electron app remains buildable and usable as the fallback throughout development and local validation.

**Hard release boundary:** the GPUI app must be built, installed, exercised, and accepted on the target machine before any GPUI release artifact is published or offered to users. Passing CI, compiling on another runner, or producing a signed DMG is not a substitute.

**Current PR #703 scope decision (2026-10-03):** at the user's explicit direction, the GPUI replacement and removal of the Electron desktop application are to be delivered in this same umbrella PR, not left as a later Electron-removal PR. Keep #703 in draft while production-equivalent GPUI host/browser/backend capabilities are implemented and verified. Remove Electron app code/dependencies only as the final migration change, after GPUI owns every required capability and passes the target-machine/local acceptance gates. Do not delete Electron early or allow this preview-only state to merge. This changes the implementation/PR boundary, not the requirement for local acceptance and an explicit release go/no-go. Preserve runtime/backend/data contracts and Electron-game compatibility (the latter means games built with Electron, not MetalSharp's desktop framework).

## Current boundary and port inventory

- The renderer is Vue 3 (`app/src/renderer`); `SharpView.vue` is about 7,900 lines, `LibraryView.vue` about 2,550, and the app includes setup, migration, settings, logs, streaming, process manager, overlays, themes, and localization.
- Electron's main process (`app/src/main/index.ts`, about 2,835 lines) owns app lifecycle, backend lifecycle, file watchers, dialogs, OAuth windows, updater handoff, external links, shortcuts, clipboard, process controls, and IPC. `preload.ts` is the renderer's native capability boundary.
- `BackendBridge` starts, checks, restarts, and stops the C backend. The renderer calls its routes through `useApi.ts`; the backend contract and existing C tests are valuable seams to preserve.
- Packaging currently assumes Electron in `app/package.json`, release CI, split bundle creation, and the `metalsharp-electron.tar.zst` artifact. These are migration work, not incidental cleanup.

## Confirmed product flow and visual architecture

The first-run experience is an ordered setup journey, not an empty-library test:

1. On first launch (`setup.json` absent or incomplete), the app may show its intro video, then presents `SetupWizard.vue`.
2. Welcome advances to Runtime. The user installs MetalSharp's Wine/graphics/Mono/FNA runtime and support assets, then installs the Wine-hosted Steam client. These are real install operations with progress polling and retry/error states; setup may also be reopened later for repair.
3. The final setup page collects the optional Steam Web API key (for account-owned games/library sync), optional TheGamesDB key (Epic artwork fallback only), and device name. Saving the Steam key starts an ownership sync; the credentials are not interchangeable and neither key is required to render the app shell.
4. Completing setup initializes Steam and Ubisoft status and loads both backend libraries. The main library is expected to populate automatically from Steam ownership and installed Steam manifests, then track later install/uninstall/download changes via `steamapps` notifications and polling. A machine without Steam or a Steam account is not a valid end-to-end library acceptance environment.
5. The Steam surface has Play and Collection pages, artwork-led featured/installed games, search/filter, game launch/configuration/status controls, plus Steam/Ubisoft launcher controls. It is not just a read-only list.

Visual identity is game-focused and data-dense: a split-image setup wizard; a compact/sidebar app shell; a Steam-style library with featured artwork, carousel and collection grid; seven selectable themes (`dark`, `light`, `skeleton`, `forest`, `orange-peel`, `dragonfruit`, `lava`); localized UI (20 locales); custom fonts/icons; theme-aware surfaces and accents. Preserve the interaction hierarchy and visual character, not only API connectivity. Source of truth: `App.vue`, `SetupWizard.vue`, `LibraryView.vue`, `Sidebar.vue`, `LibraryTopbar.vue`, `styles/variables.css`, `useTheme.ts`, `i18n.ts`, and the C setup/Steam route implementations.

## Proposed target architecture

```text
GPUI app (Rust)
  ├── UI state, navigation, localization, themes, dialogs and accessibility
  ├── typed BackendClient ───────────────> C backend at 127.0.0.1:9274
  ├── DesktopHost services
  │     ├── backend process supervision
  │     ├── filesystem watchers / native file pickers / shell / clipboard
  │     ├── OAuth and external app handoff
  │     ├── updater, installer, migration and process management
  │     └── logging, config and app lifecycle
  └── current native engine and runtime assets (unchanged)
```

Keep UI code from spawning arbitrary processes or constructing unrestricted filesystem paths. Define narrow Rust service interfaces for OS actions, validate paths and URLs at those boundaries, and make services injectable for tests. Keep backend request/response types in a dedicated crate/module with contract tests. GPUI is pre-1.0; pin a tested revision/version and isolate framework-specific code behind a small UI layer so framework updates do not infect domain and backend modules.

The browser investigation selected a purpose-scoped native macOS WebKit window rather than bundling upstream Min (which is Electron-based). `app-gpui/src/mini_browser.rs` now provides a standalone `WKWebView`/`NSWindow`, a read-only URL header, navigation controls, HTTPS host policy, ephemeral auth/help stores, and a distinct persistent GameJolt store. This remains an integration foundation: explicit connected-mode GOG/key-help actions now call it, but the default offline preview does not. A fixed, read-only Epic result-document extractor was implemented after explicit user approval; offline native-window layout/read-only/min-size/reload/close checks and user-approved anonymous Steam Store browsing passed. OAuth provider redirects remain untested. Complete callback/session design, data cleanup, security review, and local native-window acceptance before treating browser parity as done.

## Current connected integration evidence

`app-gpui/src/connected.rs` is an opt-in integration workbench, not the approved final UI. It uses the authoritative C backend for runtime/support and Steam installation/progress, Steam/Ubisoft start-stop, real library load and game install/launch-stop, persisted pipeline/executable selections, setup/key saving, GOG support/native callback handoff and Epic support/automatic result-document extraction (user-approved read-only script) with manual-code fallback. No production install/account/game actions were run. The default visual preview remains offline. Forty-five Rust tests passed; a separately invoked isolated C-backend smoke test passed for owned startup, status/PID/home identity, empty libraries and graceful shutdown/port release. Existing C tests passed (bundle-dependent runtime-ready simulation skipped). The optional connected-resource package passed plist/ad-hoc deep-signature verification. These are integration checks, not full parity, provider login acceptance, release readiness or permission to remove Electron.

The source-derived [backend route inventory](gpui-backend-route-inventory.json) tracks 258 exact routes and 21 prefix/unmatched comparisons requiring manual expansion. Its literal UI references are discovery candidates, not schema/behavior proof; request/response contracts and acceptance rows remain explicitly unverified. Regenerate/check with `python3 app-gpui/inventory-backend-routes.py [--check]` after router/caller changes.

## Phased plan and exit criteria

### Phase 0 — Baseline and requirements freeze (1–2 weeks)

1. Inventory every renderer view/component, preload capability, IPC handler, API route, app mode, user preference, shortcut, external integration, and packaging/runtime assumption.
2. Capture current main-branch behavior and create a feature-parity matrix. Separate required parity from explicitly deferred features; identify existing manual regressions and platform prerequisites.
3. Record data ownership and compatibility contracts: `~/.metalsharp`, runtime installation, Steam prefixes, emulator saves/configs, logs, caches, bundle roots/hashes, URL schemes, app identifiers, and updater state.
4. Define the test-machine procedure: current OS/hardware, available disk space, test account/games, required runtime bundles, and a recoverable backup/restore procedure.

**Exit:** reviewed inventory and parity matrix; no planned change to runtime/backend/data contracts without a written reason. The existing Electron app is still the untouched fallback.

### Phase 1 — GPUI and native-integration spike (2–3 weeks)

1. Create a separate Rust workspace/app target (do not replace `app/` in place). Build a standalone GPUI window and package/install a clearly branded local test build.
2. Validate GPUI on this machine: text/glyph rendering, input/IME, resizing, scrolling, high-DPI, keyboard navigation, native menus/dialogs, accessibility basics, and required macOS deployment target.
3. Launch the existing backend from the Rust host, wait for readiness, send typed requests to `/status` and a representative data route, and shut it down safely. Use isolated validation data; do not claim Steam-library end-to-end behavior when the test machine has no Steam/account data.
4. Prototype one visually representative, data-heavy interaction (recommended: the setup-to-library journey with deterministic mock state and representative game cards; use a real backend route separately for integration) plus a native file-picker action.
5. Continue the selected native WKWebView integration: verify the standalone window visually/behaviorally with safe test fixtures, complete callback/session design and cleanup, and review lifecycle/reopen behavior, app signing/notarization, framework pinning, and dependency licenses.

**Exit:** the app works on this machine against the existing backend; the core native integration is proven; the OAuth strategy and build/distribution approach are documented. Stop and reassess if GPUI or a required native integration cannot meet product needs.

### Phase 2 — Rust app foundation and safe backend host (3–5 weeks)

1. Implement the GPUI application shell: navigation, window/menu behavior, app lifecycle, error/toast handling, modal/overlay infrastructure, logging, and async/task boundaries.
2. Implement `BackendClient` with typed request/response models, timeouts/cancellation, error normalization, backend readiness, and route-contract tests. Preserve current route semantics and test against the existing C backend.
3. Implement `DesktopHost` interfaces for backend process ownership, app paths/config, dialogs, reveal/open, clipboard, external URLs, filesystem watchers, shortcuts, process sampling/actions, and graceful shutdown.
4. Port preference persistence, localization, theme/color tokens, icons, fonts, image loading/cache, markdown rendering, and progress/notification patterns.
5. Add development-only modes: deterministic mock API, UI-only preview, and validation configuration. Keep test-only ports and data paths isolated from normal use.

**Exit:** app shell is usable without Electron; backend can start/recover/stop; native capabilities have tests; no user-data mutation occurs in mock/preview mode; initial accessibility and keyboard-navigation conventions exist.

### Phase 3 — Port core screens and workflows (6–9 weeks)

Port behavior in user-journey order, reusing backend APIs rather than duplicating business logic:

1. First launch, dependency checks, setup wizard, install progress, Steam setup, and recovery/reopen-setup path.
2. Main Steam/Ubisoft library, artwork, install state, filtering/sorting, launch-method selection, launch settings, running status, and stop controls.
3. Settings and preferences, including paths and API keys; logs/diagnostics; update status and restart/quit flows.
4. Migration screen and migration progress/restart handoff.
5. Streaming/Sunshine status and controls; process manager overlay, sampling/actions, global shortcuts, and force-quit behavior.
6. Toasts, launch overlay, artwork refresh notifications, Steam library watchers, and all empty/loading/error states.

**Exit:** feature matrix records parity for these journeys; request payloads/results match the existing backend contract; tests cover navigation, state transitions, error paths, and first-run/setup recovery.

### Phase 4 — Port Sharp Library and provider/emulator surface (6–10 weeks)

1. Break the existing monolithic Sharp Library behavior into explicit domain modules before or during the port (provider state, game list/card, install/update tasks, process supervision, emulator configuration, dialogs, and progress events).
2. Port GameJolt, GOG, Epic, SharpEmu, RPCS3, PCSX2, and shadPS4 library/setup/update/launch flows and all related status/progress/error states.
3. Port GOG/Epic login with the chosen native/external browser integration. Validate callback handling, cancellation, state/CSRF protections, host allow-lists, and secret handling.
4. Port provider-specific paths, file pickers, guides, compatibility links, firmware/game selection, updater/recovery UI, and process polling/notifications.
5. Verify no emulator save, firmware, library, or configuration path is reset, moved, or overwritten by UI startup or upgrade.

**Exit:** provider-by-provider parity matrix is complete; no secrets are logged; OAuth and file/path boundaries pass security review; representative install/update/launch/stop flows work against the existing backend.

### Phase 5 — Full native packaging, installer, and CI (4–7 weeks)

1. Build an arm64 macOS app bundle/DMG for the same supported macOS baseline; integrate app icon, URL scheme, entitlements, hardened runtime, deep signing, notarization, and verification.
2. Replace Electron payload assumptions in app packaging, bundle creation/manifests/verifiers, installer scripts, docs, and release artifact naming. Keep runtime, graphics, assets, Steam and script-tools bundles separate and unchanged where possible.
3. Preserve the C backend and native engine build/package steps. Test resource lookup from both development build and installed `.app`; confirm executables, dylibs, helper tools, licenses, permissions, hashes, and bundle roots.
4. Add Rust formatting/lint/unit tests and GPUI integration tests to CI. Add a packaging smoke test, but keep publication gated separately.
5. Keep Electron CI/build available while GPUI parity is being implemented. For PR #703, after production GPUI replacements and local acceptance pass, remove the Electron desktop application source/dependencies/build integration in a final, separately reviewable commit within this same draft umbrella PR; delete nothing early. Retain Electron CI only as long as other branches need it, then remove/update it with the app migration.

**Exit:** a clean checkout can build the app; the packaged app launches without a development tree; static bundle/signing/notarization verifiers pass; no release upload is enabled for the GPUI artifact.

### Phase 6 — Local-machine installation and acceptance (2–4 weeks minimum)

This phase must happen on Avery's current machine using the actual packaged GPUI build. It is a required release prerequisite, not an optional soak test.

1. Back up the existing MetalSharp app and user data; verify restore instructions before installing anything.
2. Install the GPUI app as a side-by-side candidate with a distinct local-test app identifier/name and isolated validation home/config where practical. It must not replace or silently update `/Applications/MetalSharp.app`, claim production URL handlers, or alter production updater state during qualification.
3. Run the full automated suite on the target machine, then execute the manual parity matrix: first run, setup/repair, library refresh and artwork, launch/stop representative Steam and Sharp titles, emulator/provider sign-in and representative launch flows, logs, settings, streaming, shortcuts/process manager, quit/reopen, migration/recovery, and updater/install handoff.
4. Test with both clean test data and a backed-up copy of real user configuration where safe. Specifically verify paths, prefixes, saves, firmware, caches, installed runtimes, and settings remain intact. Exercise offline/backend-unavailable and interrupted-install/update cases.
5. Keep the candidate app installed and use it for an agreed soak period (minimum 14 consecutive days; target 30 days), record defects, fix them, and rerun the affected acceptance cases. A failed test returns to implementation; it does not waive the gate.
6. Leave Electron installed and usable during qualification. At the end, verify downgrade/rollback by reopening Electron and confirming it can still access the existing data and launch a known-good title.

**Exit:** signed acceptance record includes machine/OS/build/commit identifiers, test results, known limitations, soak dates, data-integrity checks, backup/restore result, and explicit owner approval. No unresolved blocker or data-loss/security issue remains. Without this record, do not release.

### Phase 7 — Release decision and controlled cutover (separate approval; distinct from code-removal timing)

Only after Phase 6 passes, make a separate go/no-go decision. Review app identity, URL scheme ownership, update channel, artifact/bundle compatibility, support/rollback plan, and versioning. The user's same-PR Electron removal request does not itself authorize release/publication. Enable GPUI publication only in this phase, with explicit workflow review and fresh end-to-end release-candidate testing. Preserve a tested rollback to the last known-good Electron release and existing `~/.metalsharp` data.

**Exit:** explicit approval to publish; release workflow is intentionally changed and reviewed; published artifact matches the locally accepted commit and configuration. If approval is not given, retain the GPUI app as a local/test build and keep Electron as the released application.

## Validation strategy

- **Backend contract:** existing C tests remain authoritative for backend behavior; add Rust serialization/request tests and a GPUI-to-local-backend smoke suite for every route family used by the UI.
- **UI behavior:** test view navigation, async loading/error/retry, cancellation, progress, empty states, keyboard shortcuts, dialog return values, accessibility labels/focus, and mock-mode isolation.
- **Native lifecycle:** backend absent/busy/slow/crashed; app quit/reopen; duplicate-app prevention; watchers; global-shortcut conflicts; updater handoff; cleanup of child processes.
- **Data safety:** backup/restore before upgrade; no mutation during mock mode; preserve established data locations and emulator/Steam state; test downgrade to Electron.
- **Artifact:** clean-build installed app, architecture and deployment-target verification, embedded-resource and executable checks, deep codesign, notarization, DMG mount/install/launch, and offline first launch.
- **Local gate:** all automated and manual cases run on the actual target machine. Remote CI results are necessary but insufficient.

## Indicative schedule and risks

The phases imply roughly **24–40 engineer-weeks** of work (about **6–10 months for one experienced Rust/GPUI engineer**; roughly **3–6 calendar months with a small team**, assuming integration work can run in parallel). The local soak period is additional elapsed time. Estimates should be revised after the spike and inventory.

Largest risks:

- GPUI's pre-1.0 API churn and still-evolving examples/tooling.
- The conversion cost of bespoke UI behavior concentrated in `SharpView.vue` and other large Vue components.
- Native replacement of Electron webviews/OAuth, file dialogs, app restart/updater, auxiliary windows, and global shortcuts.
- UI responsiveness against a backend whose HTTP server handles one request at a time; request scheduling and cancellation must not make library scans or progress polling worse.
- Bundle and installer assumptions that call the app payload "Electron" and are covered by release-specific verifiers.
- macOS accessibility, text input, native appearance, signing, and installed-app resource lookup not being proven by a developer-only window.
- Testing destructive setup, migration, updater, and game operations safely on a machine with real user data.

## Source map

- App structure and contracts: `AGENTS.md`
- Electron lifecycle/backend startup: `app/src/main/index.ts`, `app/src/main/backend-bridge.ts`
- Renderer/native API seam: `app/src/main/preload.ts`, `app/src/renderer/composables/useApi.ts`, `app/src/renderer/api-types.ts`
- UI screens: `app/src/renderer/App.vue`, `app/src/renderer/views/`, `app/src/renderer/components/`
- Backend API: `app/src-c/runtime/backend.c`, `app/src-c/runtime/http_server.c`, `app/src-c/tests/`
- Packaging/release: `app/package.json`, `.github/workflows/release.yml`, `tools/bundles/`, `tools/dmg/`
- GPUI upstream: [GPUI](https://gpui.rs/) and [GPUI README](https://github.com/zed-industries/zed/blob/main/crates/gpui/README.md). Upstream currently identifies GPUI as pre-1.0 and in active development; re-check compatibility and platform requirements before pinning a revision.
