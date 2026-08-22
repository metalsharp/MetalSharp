# Hand-rolled C backend

This directory is the owned C implementation of `metalsharp-backend`. It is
not generated C and it does not depend on a Rust-to-C compiler.

The current hand-written slice owns the process boundary, readiness contracts, and the ported stateful service domains below. It is still being expanded toward complete Rust parity:

- executable name: `metalsharp-backend`;
- backend filename: `metalsharp-backend`;
- integration/install output path: `app/src-rust/target/release/metalsharp-backend`;
- packaged resource path: `runtime/metalsharp-backend`;
- `METALSHARP_PORT`, `METALSHARP_HOME`, `METALSHARP_DEV`, and
  `METALSHARP_STEAM_BRIDGE_PORT` behavior;
- `GET /status` and `GET /runtime/host-abi`;
- configuration, setup state/progress, updater progress, migration inspection,
  cache, logs, MetalFX, scanning, Steam library/API-key/status basics, and MTSP
  catalog routes;
- virtual kernel-translation handle tables, APC queues/context/trampoline
  state, code-integrity module state, driver/device/IRP/IOCTL state, anti-debug
  checks, Object Manager callbacks, EndpointSecurity events/IPC, thread
  watchers, integration lifecycle/config/log state, and core process/launch/
  Steam action routes;
- a persistent hand-written Sharp Library manifest at
  `METALSHARP_HOME/sharp-library/library.json`, including import/install,
  listing, validation, and uninstall behavior;
- a persistent GOG library/authentication marker under
  `METALSHARP_HOME/gog/`, plus prefix initialization and game import state;
- persistent D3DMetal GPTK bottle state under
  `METALSHARP_HOME/d3dmetal-gptk/bottles/<id>/state.json`, restored across
  backend restarts;
- persisted Goldberg emulator enablement markers under
  `METALSHARP_HOME/goldberg/<appid>/`;
- `/setup/install-deps` executes supported Homebrew dependency installs
  (`mono` and `sdl3`) with real exit-status reporting;
- bottle compatibility-matrix and redistributable-source routes expose the
  same baseline cases and official source guidance as Rust; compatibility
  evidence updates persist under `METALSHARP_HOME/bottles/`;
- `/bottles/sync-steam` discovers Steam ACF games and writes bottle manifests
  with stable `steam_<appid>` IDs;
- bottle refresh, runtime-profile selection, edit, and Windows-version routes
  now read and persist manifest state instead of returning compatibility-only
  acknowledgements;
- bottle doctor reports inspectable prefix, component, app-detection, and
  game-runtime checks;
- `/bottles/verify-directx` inspects the expected DirectX June 2010 DLL set
  across both Wine architectures;
- bottle preparation, dry-run component repair, font substitutions, and
  post-wineboot seeding now perform filesystem/Wine operations and return PIDs;
- Windows-version changes launch the bundled Wine `reg add` operation and
  write bottle-scoped logs with the child PID.

This list is not a parity claim: the remaining Rust side-effect domains must
still be ported before the C backend can replace Rust. Unported kernel,
bottle, and installer requests currently have explicit compatibility responses
so the HTTP surface remains stable, but those responses are not a substitute
for the Rust implementations.

The output path intentionally retains the repository's existing backend path so
Electron, the DMG scripts, and updater discovery do not silently change while
service domains are ported one at a time. The Rust backend remains the active
build until the C implementation has equivalent route and state coverage.

Build and smoke-test from this directory:

```sh
make
make test
make asan-test
```
