# MetalSharp GPUI preview

This is an isolated GPUI UI-parity preview for the main Library Play/Collection views, theme picker, and library header/footer. It opens on Play with a bundled 20-game sample library with distinct portrait covers and hero images; no Steam install, account, backend, production data, or network access is used at runtime. Header navigation, theme selection, local sample-game selection, and the setup preview are interactive. Header menus are button-relative overlays rendered above the hero, and search flexes within a 720 px maximum width with 20 px minimum space on both sides. Search is a native focusable text input with selection, clipboard editing, horizontal caret scrolling, and input-method handling; it filters the bundled preview games locally in Play and Collection. Light mode uses black input text/placeholder, white dropdowns with black text, white header/footer button outlines, and creamy-white dock glow. Skeleton, Forest, and Orange Peel use the original UI's Lucide bone, pine tree, and citrus icons. The hero settings popover provides per-sample-game MetalFX, controller input, and Msync selections plus a native Choose EXE picker. The Bottle control opens a selectable six-pipeline dropdown, with its selection stored per sample game. Card underglow uses blurred theme-colored shadows rather than solid rectangles. The hero image fills the resized hero bounds with aspect-preserving cover cropping. Dock cards scale to window width; selecting one raises it by 16 px from its normal slot. View All stays 32 px below the hero controls. The shallow arch and card angles are fixed around the dock's midpoint, independent of selection; selecting an end card does not tilt the rest of the row toward that end. Dock arrows scroll a five-card window through all 20 entries and stop at each end. The arch and artwork tilt are slot-relative, so adding entries does not reshape the fan. Choosing Play from Collection brings that game's dock window into view. Collection wraps proportionally sized cards into rows and scrolls independently of the fixed header/footer. The minimum preview window is 720×540. At narrow widths search displays only “Search”; Collection's Back to Play stays at the right of its heading. The footer Stream button opens the streaming panel, matching the Electron panel's Sunshine/Pair Device/Good to know sections, styles, copy, and scroll layout. Install/Start/Stop only change preview-memory state; PIN entry, pairing, web links, and device removal are disabled or inert. No Sunshine download, service launch, networking, or credential transmission occurs. Settings and selected paths exist only in preview memory; the picker never reads executable contents, runs the file, or saves to backend/production settings. Game launch, Steam actions, updater, search, and configuration controls do not call services or alter user data. Sharp Library now has all eight source previews, a button-anchored two-column source picker, source-local sample state, shared launch preferences, emulator overview/sidebar controls, and safe simulated dialogs. Its workspaces stretch vertically toward the footer; page-header controls use 12 px spacing. GameJolt's resizable browser frame is explicitly disabled offline. Logs has synthetic Live/Crash Reports/Recent Files drawers, nested scrolling, clipboard Copy, and memory-only Clear View. These pages still have documented visual and workflow gaps; see `PAGE-PARITY.md`. The approved eight-card Settings overlay opens directly from the header gear; credentials remain non-editable placeholders and dangerous actions require safe synthetic confirmations. See `SETTINGS-PARITY.md`. The three-page setup preview is available from Settings → Run Setup Wizard; its install actions are simulated and its fields remain visual placeholders.

Artwork for the additional 15 games comes from public Steam CDN library images; source URLs are recorded in `assets/preview-artwork-sources.json`. Artwork belongs to the respective game publishers and is bundled for this isolated local visual test, not a redistribution license or a runtime compatibility claim.

## Build and run from the repository

```sh
(cd app-gpui && swift generate-dock-art.swift)
cargo run --locked --manifest-path app-gpui/Cargo.toml
```

This preview is deliberately isolated, does not access production app data, and does not require Steam to be installed. Main-library artwork and setup assets are bundled locally, so the preview makes no runtime requests for game art. Run it with:

```sh
cargo run --locked --manifest-path app-gpui/Cargo.toml
```

## Opt-in connected integration candidate (not full parity)

The default app remains the approved offline preview. Explicit connected modes use a separate integration workbench, **not the final visual-parity UI**:

```sh
make -C app/src-c
cargo run --locked --manifest-path app-gpui/Cargo.toml -- --connected-validation
# Approved setup layout, driven by the same real isolated backend:
cargo run --locked --manifest-path app-gpui/Cargo.toml -- --connected-setup-validation
```

Validation defaults to port 9276 and a dedicated temporary data home. Override with `METALSHARP_GPUI_PORT`, `METALSHARP_GPUI_HOME` (absolute dedicated directory), and optionally `METALSHARP_GPUI_BACKEND`. Production port/home, parent traversal, and user-created symlink ancestors are rejected for validation. A distinct `--connected-production` flag opts into port 9274 and `~/.metalsharp`; **do not use it before backup and a user-approved acceptance plan**.

Connected buttons send real C-backend requests for runtime/support installation and progress, Steam installation/status, Steam/Ubisoft start-stop, API-key save/sync, setup completion, library load, game install/launch-stop, and persisted pipeline/executable selections. They are not simulations. Isolating the data home is not OS sandboxing: installers and launchers can still interact with system dependencies. No installation, account login, API-key save or game launch has been exercised against real data during development.

GOG initialization/sign-in routes and the native callback-to-backend handoff are wired; Steam/TheGamesDB key-help links open the native browser. Epic support installation and automatic authorization-code completion are wired using the explicitly user-approved fixed, read-only extractor at the exact Epic JSON redirect endpoint; manual code submission remains a fallback. Native URL/document/generation checks reject stale or off-origin results, and bounded parsing rejects ambiguous code fields. Credentials clear from input controls after dispatch and errors do not print request/response bodies. Credential inputs are bullet-masked, IME queries return masked text, and secret copy/cut cannot export values. Provider live acceptance, progress/timeout UX, automatic host crash recovery (explicit owned-backend restart, child-liveness gating and isolated restart/crash ownership smoke tests are implemented), full Settings/Sharp/Logs integrations, production artwork and the approved library presentation remain required. The approved streaming panel now uses actual status/install/start/stop/PIN/unpair routes in connected modes; ordinary preview controls remain simulated. Its PIN is masked and cleared on dispatch/close; unpair requires a second confirmation. Polling is panel-scoped, and only an explicitly accepted installation can trigger one automatic host start on completion. Closing/reopening cancels that intent and stale responses. The fixed Sunshine Web UI opens in the system browser, never through a WebKit local-network-filter exception. Independent review led to fail-closed status invalidation after backend exit/mutation errors, expiring confirmation reset on lost readiness, and a post-completion polling cooldown (15 seconds normally, 3 during installation) so a slow Sunshine probe does not immediately monopolize the shared backend lane. A real-C isolated smoke read also verifies the streaming schema and session-header boundary without installing or starting Sunshine. Real service installation, permissions, credential recognition and device pairing remain unaccepted. Do not treat this workbench as parity completion or remove Electron.

The connected setup command binds the approved three-page layout to real progress, dependency readiness, Steam status, editable device name, masked optional keys and backend completion. Missing readiness or migration requirements block advance/finalization; it never switches to the sample library on completion (the connected workbench follows until final-library integration is complete). Default launch remains unchanged and synthetic. Runtime preferences use partial `/config` updates that preserve unknown keys; connected diagnostics load on demand with bounded, credential-marker-redacted lines. Arbitrary third-party log safety is not guaranteed.

GPUI-owned backend sessions require a fresh 256-bit per-launch header token. The backend consumes/unsets its token before helpers or games start, rejects unauthenticated requests before reading bodies/dispatching routes, and omits CORS permissions in this mode. Legacy standalone/Electron sessions retain their existing API behavior. Browser documents load only after WebKit resource-blocking rules compile/install: HTTP/WS, file resources, private IPv4, IPv6 literals and local DNS names are denied. This conservative filter does not inspect DNS-resolution results; backend authentication is the independent app-data boundary. No token enters pages, URLs or logs.

Offline validation: 67 Rust tests passed (including authenticated streaming transport, nullable status/redaction, PIN rejection before transport, install-completion/cancellation and unavailable-host virtual-window checks), and the separately invoked ignored C-backend smoke test passed against a fresh temporary home (startup, PID/home ownership, empty Steam/Ubisoft libraries, graceful shutdown and port release, unauthenticated reads/writes rejected, session-token rotation, partial config persistence and unknown-key retention, bounded/redacted logs). Native connected SIGTERM also reaped its owned backend and closed its listener. Existing C tests passed; bundle-dependent runtime-ready simulation was skipped because archives were absent. These tests do not install Steam or validate real accounts/games.

## Native browser acceptance harness

A build-only feature exposes explicit browser checks; ordinary packaging does not enable it. Generate dock artwork as above before building.

```sh
cargo run --manifest-path app-gpui/Cargo.toml --locked --features browser-fixture -- --browser-fixture
# Live public browsing, only when explicitly requested:
cargo run --manifest-path app-gpui/Cargo.toml --locked --features browser-fixture -- --browser-steam-test
```

The offline command supplies fixed compiled HTML with a restrictive CSP; subsequent navigation is denied, the header explicitly says OFFLINE, and no backend/account/data home is opened. The optional `--browser-network-fixture` command requires `METALSHARP_BROWSER_PROBE_PORT` pointing at a dedicated dummy HTTP server (not a backend port). Its fixed, script-free HTML probes loopback/localhost/numeric-alias image requests; native testing recorded zero page requests and a successful separate server-positive-control request. It must not target a real service.

The live command opens `https://steampowered.com` with a fresh nonpersistent store and an exact three-host Steam Store navigation policy, separate from API-key help. Quit the test app afterward.

Local checks: native HTML rendered, read-only header verified, minimum resize clamped to 720×540, offline reload and close exercised. Live Steam Store redirected/rendered at `https://store.steampowered.com/`, including with compiled resource-blocking rules installed; the user approved the browser's look. No account login or backend/install action was performed. This does not establish OAuth/provider or full production parity.

The offline preview's macOS 13 bundle minimum is not proof of connected-mode deployment compatibility: the current C build targets macOS 14, and named GameJolt storage also requires 14. Resolve and test the production deployment target before release; no support-policy change is made by this preview.

## Package a side-by-side local candidate

```sh
app-gpui/package-local-preview.sh
open app-gpui/target/MetalSharp-GPUI-Preview.app
```

For a versioned local preview, set `METALSHARP_GPUI_PREVIEW_VERSION` (for example, `0.77.0`); this only changes the local app bundle's version metadata.

To bundle the authoritative C backend, compression/icon tools, existing available runtime archives, config and updater resources for connected testing:

```sh
METALSHARP_GPUI_PACKAGE_BACKEND=1 app-gpui/package-local-preview.sh
```

This only builds/copies resources; it does not run installers or rebuild runtime archives. The connected package is ad-hoc signed and verified locally, but production resource completeness and release acceptance are still outstanding.

The candidate has a separate `dev.metalsharp.gpui-preview` bundle identifier and is ad-hoc signed for local testing only. It is not notarized, published, or suitable for distribution. It bundles its UI image assets in `Contents/Resources/assets/` and makes no backend or production-data changes. The app icon source is `assets/metalsharp-liquid-glass.icon`, authored with Apple Icon Composer; packaging compiles its Liquid Glass `Assets.car` and legacy `.icns` with `actool` from full Xcode. If Xcode is not selected, set `DEVELOPER_DIR` to the Xcode bundle's `Contents/Developer` directory.

The native input in `src/search_input.rs` is adapted from GPUI 0.2.2's Apache-2.0 `examples/input.rs`; license included in `assets/gpui-input-LICENSE-APACHE`. Rethink Sans fonts retain their SIL Open Font License in `assets/RethinkSans-OFL.txt`. Theme SVGs come from Lucide's `bone`, `tree-pine`, and `citrus` icons under the ISC license in `assets/lucide-LICENSE.txt`.

## Dock artwork

GPUI 0.2.2 does not expose raster-element rotation. The isolated sample preview uses transparent tilted cover variants in the git-ignored `assets/dock/`, generated from the original local covers without changing them. Packaging regenerates these variants automatically. Before the first `cargo run`, generate them once:

```sh
cd app-gpui
swift generate-dock-art.swift
```

This is a sample-artwork preview workaround, not a completed dynamic production-library rendering path.

## GPUI shader build

The manifest pins GPUI `0.2.2` and enables its `runtime_shaders` feature. This avoids requiring the offline `metal`/`metallib` Xcode tools during compilation; runtime shader initialization must still be validated on the target Mac. Before a distribution build, validate with a supported full Xcode toolchain and decide whether offline-compiled shaders are required.
