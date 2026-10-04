# MetalSharp GPUI app

Native GPUI replacement for the Electron desktop app. A normal launch is the production app, exactly like Electron: it starts the packaged C backend on `127.0.0.1:9274` with `~/.metalsharp` (an orphaned or older `metalsharp-backend` on that port is terminated first, then a fresh one is spawned without a session token so `update.sh` and other local helpers can query `/status`), opens the setup wizard when `setup.json` is not completed or the runtime needs migration, and otherwise shows the library. Quitting the app stops the backend.

Behaviour is ported 1:1 from the Electron renderer and main process (removed from the repository once the port was complete; the sources below are in git history):

| Area | GPUI module | Electron source |
|---|---|---|
| Startup, migration mode, health polling, library load/merge, steamapps + grid-art watching, Play/Stop/running poll, hero settings (MetalFX, controller, msync, EXE, Steam Emu), Bottle selection, Steam/Ubisoft launchers, Fix Steam, setup wizard, updater, streaming, ⌘⌥Q force-quit | `src/ui.rs`, `src/ui_live.rs`, `src/library_model.rs` | `App.vue`, `LibraryView.vue`, `LibraryTopbar.vue`, `SetupWizard.vue`, `StreamingOverlay.vue`, `main/index.ts` |
| Settings overlay | `src/settings_preview.rs` | `SettingsOverlay.vue` |
| Sharp Library (Installers incl. Launch Doctor/D3DMetal launch, GOG, Epic, GameJolt split view, PCSX2, RPCS3, shadPS4, SharpEmu) | `src/sharp_preview.rs`, `src/sharp_live.rs`, `src/sharp_live_view.rs`, `src/sharp_tools.rs`, `src/sharp_tools_view.rs`, `src/sharp_emu_live.rs` | `SharpView.vue`, `main/index.ts` OAuth/download handlers |
| Logs | `src/logs_preview.rs` | `LogsView.vue` |
| Backend lifecycle and requests | `src/backend_host.rs`, `src/live.rs` | `main/backend-bridge.ts`, `composables/useApi.ts` |
| Updater handoff | `src/updater_bridge.rs` | `main/updater-bridge.ts` |
| Artwork (Steam CDN, grid art, store details, SteamGridDB, tilted dock covers) | `src/artwork.rs` | `LibraryView.vue` artwork probing, `generate-dock-art.swift` |
| Steam Art Manager, folders, data-access repair, uninstall, GameJolt download organizing | `src/host_actions.rs` | `main/index.ts` IPC handlers |
| Launch overlay / global shortcut | `src/launch_overlay.rs`, `src/hotkeys.rs` | `showLaunchOverlay`, `registerForceQuitGamesShortcut` |
| Toasts | `src/toast.rs` | `useToast.ts`, `Toast.vue` |
| First-launch intro video | `src/intro_video.rs`, `assets/intro/intro.html` | `App.vue` startup video |
| ⌘P Process Manager HUD (`--process-manager-overlay` opens only the HUD) | `src/process_manager.rs` | `ProcessManagerOverlay.vue`, `process-manager:*` IPC |

GOG/Epic sign-in use the native WebKit browser window in `src/mini_browser.rs`, and the GameJolt store is the same WebKit view embedded in the Sharp page; GameJolt downloads are saved to `<GameJolt>/.downloads`, extracted into `<GameJolt>/<name>/` and synced, as Electron's `persist:gamejolt` session did.

## Build and run from the repository

```sh
make -C app/src-c
(cd app-gpui && swift generate-dock-art.swift)
cargo run --locked --manifest-path app-gpui/Cargo.toml                          # production: :9274, ~/.metalsharp
cargo run --locked --manifest-path app-gpui/Cargo.toml -- --connected-validation   # isolated home on :9276
cargo run --locked --manifest-path app-gpui/Cargo.toml -- --preview                # offline sample UI
```

Validation mode uses `METALSHARP_GPUI_PORT` (default 9276), `METALSHARP_GPUI_HOME` (absolute dedicated directory) and optionally `METALSHARP_GPUI_BACKEND`, and protects the owned backend with a per-launch session token. Production data and port are rejected for validation.

## Package

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer app-gpui/package-app.sh
```

`package-app.sh` builds a release `MetalSharp.app` with the Electron app's identity (`com.metalsharp.app`, `Contents/MacOS/MetalSharp`, version from `CMakeLists.txt`, `metalsharp://` URL type), bundling the C backend, host runtime, tools, scripts/updater, configs and the six runtime archives from `app/bundles`. That identity is what `app/updater/update.sh` quits, verifies and replaces. The output is ad-hoc signed for local testing; release signing/notarization stays in the release workflow.

## Native browser acceptance harness

A build-only feature exposes explicit browser checks; ordinary packaging does not enable it. Generate dock artwork as above before building.

```sh
cargo run --manifest-path app-gpui/Cargo.toml --locked --features browser-fixture -- --browser-fixture
# Live public browsing, only when explicitly requested:
cargo run --manifest-path app-gpui/Cargo.toml --locked --features browser-fixture -- --browser-steam-test
```

The offline command supplies fixed compiled HTML with a restrictive CSP; subsequent navigation is denied, the header explicitly says OFFLINE, and no backend/account/data home is opened. The optional `--browser-network-fixture` command requires `METALSHARP_BROWSER_PROBE_PORT` pointing at a dedicated dummy HTTP server (not a backend port). Its fixed, script-free HTML probes loopback/localhost/numeric-alias image requests; native testing recorded zero page requests and a successful separate server-positive-control request. It must not target a real service.

The live command opens `https://steampowered.com` with the persistent Steam browser store and an exact three-host Steam Store navigation policy, separate from API-key help. Quit the test app afterward.

Local checks: native HTML rendered, read-only header verified, minimum resize clamped to 720×540, offline reload and close exercised. Live Steam Store redirected/rendered at `https://store.steampowered.com/`, including with compiled resource-blocking rules installed; the user approved the browser's look. No account login or backend/install action was performed. This does not establish OAuth/provider or full production parity.

The offline preview's macOS 13 bundle minimum is not proof of connected-mode deployment compatibility: the current C build targets macOS 14, and named GameJolt storage also requires 14. Resolve and test the production deployment target before release; no support-policy change is made by this preview.

## Side-by-side test package

`package-local-preview.sh` (debug, `MetalSharp-GPUI-Preview.app`) and `package-local-testing-app.sh` / `package-local-testing-dmg.sh` (release, `MetalSharp-GPUI-Test.app`) build the same production app under separate bundle identities. They use the real `~/.metalsharp` and port 9274, so quit the Electron app first.

## Dock artwork

GPUI 0.2.2 does not expose raster-element rotation. The isolated sample preview uses transparent tilted cover variants in the git-ignored `assets/dock/`, generated from the original local covers without changing them. Packaging regenerates these variants automatically. Before the first `cargo run`, generate them once:

```sh
cd app-gpui
swift generate-dock-art.swift
```

Real library covers get the same tilt at runtime: `artwork::render_tilted_card` reproduces this rotation for downloaded covers and caches the variants under `~/.metalsharp/cache/gpui-artwork/`.

## GPUI shader build

The manifest pins GPUI `0.2.2` and enables its `runtime_shaders` feature. This avoids requiring the offline `metal`/`metallib` Xcode tools during compilation; runtime shader initialization must still be validated on the target Mac. Before a distribution build, validate with a supported full Xcode toolchain and decide whether offline-compiled shaders are required.
