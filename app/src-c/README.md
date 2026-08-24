# Hand-rolled C backend

This directory is the owned C implementation of `metalsharp-backend`. It is
not generated C and it does not depend on a Rust-to-C compiler.

The current hand-written slice owns the process boundary, readiness contracts, and the ported stateful service domains below. It is still being expanded toward complete Rust parity:

- executable name: `metalsharp-backend`;
- backend filename: `metalsharp-backend`;
- integration/install output path: `app/src-c/build/metalsharp-backend`;
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
  write bottle-scoped logs with the child PID;
- isolated PCSX2 environment management with Intel/SSE4.1 and Apple Silicon/Rosetta gates, official stable `.tar.xz` size/digest and path-safety checks, preserved Developer ID/notarization, atomic update/rollback, validated user-owned BIOS import, bounded disc-image/homebrew discovery, and restart-safe supervision. See `docs/emulators/PCSX2-INTEGRATION.md` for the production contract;
- isolated RPCS3 environment management, official architecture-specific
  release checks, digest/signature-verified atomic updates and rollback,
  firmware/PKG installation, `PARAM.SFO` game discovery, artwork, launch logs,
  and restart-safe process supervision. See `docs/emulators/RPCS-INTEGRATION.md`
  for paths, security rules, and API details;
- experimental shadPS4 management for supported Apple Silicon hosts, including
  Rosetta/macOS readiness gates, official stable ZIP size/digest verification,
  safe extraction, Mach-O/ICD/CLI validation, local ad-hoc signing, atomic
  rollback, bounded CUSA discovery, optional console-dumped module/font import,
  and restart-safe launch supervision. See
  `docs/emulators/SHADPS4-INTEGRATION.md` for the production contract;
- experimental SharpEmu PlayStation 5 research environment management with
  macOS/Rosetta/deployment-target gates, exact GitHub release and mutable-asset
  identity checks, bounded tar extraction, recursive Mach-O validation, local
  ad-hoc signing, read-only atomic versions, isolated saves/caches/logs,
  bounded `param.json` discovery, network denial by default with confirmed
  per-launch opt-in, and restart-safe process supervision. See
  `docs/emulators/SHARPEMU-INTEGRATION.md` and
  `docs/emulators/SHARPEMU-UPSTREAM-CONTRACT.md`.

This list is the active C backend's implementation inventory. Rust remains
available temporarily as a differential parity oracle, but it is no longer the
application's build, package, or runtime reference. Any remaining compatibility
responses are tracked as C parity work rather than delegated to Rust at runtime.

The backend filename and packaged resource path remain stable so Electron, the
DMG scripts, and updater discovery do not change when the implementation changes.
The C backend is now the active build and runtime reference.

Build and smoke-test from this directory:

```sh
make
make test
make asan-test
```

Compare all Rust-discovered routes against a separately built Rust reference;
semantic mode masks only process-generated values and requires zero remaining
mismatches:

```sh
make RUST_BACKEND=/path/to/rust/metalsharp-backend parity-test
```

For raw diagnostics, preserving generated PIDs, paths, timestamps, ports, and
addresses, run the comparator directly with `--raw`:

```sh
python3 tests/differential_parity.py --c build/metalsharp-backend \
  --rust /path/to/rust/metalsharp-backend --raw
```
