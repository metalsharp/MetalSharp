# shadPS4 integration roadmap

Status: **planned; not yet a supported MetalSharp provider**

MetalSharp will identify this provider by its accurate upstream name: **shadPS4**.

This roadmap adds an isolated, MetalSharp-managed PlayStation 4 environment without downloading or bundling Sony firmware modules, fonts, trophy keys, games, updates, DLC, licenses, or decryption material. shadPS4 remains early software, so the shipped UI must describe it as experimental and must not imply broad game compatibility.

## Research snapshot

Validated against upstream on 2026-08-23:

- Core project: <https://github.com/shadps4-emu/shadPS4>
- Stable releases: <https://github.com/shadps4-emu/shadPS4/releases>
- macOS prereleases: <https://github.com/shadps4-emu/shadps4-binaries-Mac/releases>
- User guide: <https://github.com/shadps4-emu/shadPS4/wiki/I.-Quick-start-%5BUsers%5D>
- Compatibility database: <https://github.com/shadps4-compatibility/shadps4-game-compatibility>
- License: GPL-2.0-or-later

The current stable release is `v.0.18.0`. Its official macOS core asset:

- is a ZIP containing the SDL core, Vulkan loader, and KosmicKrisp driver;
- publishes an asset size and SHA-256 digest through GitHub's release API;
- contains x86_64 Mach-O binaries and runs on Apple Silicon through Rosetta 2;
- declares macOS 26.0 as its minimum deployment target;
- is not code-signed upstream;
- exposes CLI operations including direct game launch, fullscreen mode, a persistent game-folder command, and a guest-root override.

Upstream's README and wiki currently disagree about the minimum supported macOS version. MetalSharp must derive readiness from the downloaded Mach-O deployment target and an executable probe rather than trusting prose documentation.

## Product decisions

1. **Use the official SDL core, not QtLauncher.** MetalSharp already owns library and setup UX. Adding a second launcher would duplicate state and weaken process supervision.
2. **Stable channel first.** The official `shadPS4` stable release feed is the only default update source. Nightly support is deferred until stable updates and rollback are proven.
3. **Apple Silicon only at launch.** The current macOS binary is x86_64 but explicitly requires Rosetta translation on an Apple Silicon host. Intel Macs fail closed.
4. **No dormant provider.** Do not add shadPS4 to `/emulators`, preload contracts, or the Sharp Library until its runtime, discovery, launch, stop, and preservation gates pass.
5. **No package extraction in MetalSharp.** The first release accepts already dumped game directories from a user's own games. PKG decryption, entitlement handling, and unofficial extractors are out of scope.
6. **No Sony content acquisition.** MetalSharp may copy user-selected, console-dumped modules or fonts into isolated state after validation. It will never fetch them.
7. **Preserve all user state.** Runtime removal and update rollback must retain saves, trophies, settings, controller profiles, shader caches, screenshots, patches, cheats, modules, fonts, and external games.
8. **Treat compatibility data as advisory.** Upstream compatibility labels may be displayed later but can never block a local launch or be treated as trusted executable content.

## Target filesystem model

```text
~/.metalsharp/emulators/shadps4/
├── current -> versions/<release-tag>
├── previous -> versions/<release-tag>
├── versions/<release-tag>/
│   ├── shadps4
│   ├── libvulkan.dylib
│   ├── libvulkan_kosmickrisp.dylib
│   ├── kosmickrisp_mesa_icd.json
│   ├── LICENSE
│   └── source.json
├── home/
│   └── Library/Application Support/shadPS4/
│       ├── config.json
│       ├── home/
│       ├── sys_modules/
│       ├── fonts/
│       ├── cache/
│       ├── shader/
│       ├── trophy/
│       ├── custom_configs/
│       ├── patches/
│       └── cheats/
├── downloads/
├── staging/
├── sessions/
├── logs/
├── environment.json
└── library.json
```

Launches use the selected version directory as the working directory so the relative KosmicKrisp ICD path remains valid. MetalSharp sets `HOME` to the environment's isolated `home` directory before process creation. The shadPS4 `--override-root` flag is a guest filesystem option and must not be mistaken for the host user-data root.

External game roots remain references. Removing a root from MetalSharp only updates `library.json`; it never deletes or modifies the user's game directory.

## Phase 0 — freeze and test the upstream contract

Deliver a non-shipping contract probe before adding provider code.

- Record the exact stable release repository, tag format, asset naming, API digest field, and redirect policy.
- Download a stable macOS asset into a temporary test location.
- Verify ZIP structure, SHA-256, Mach-O architecture, minimum OS, linked libraries, unsigned-signature state, and `--help` output.
- Confirm the environment variables required by KosmicKrisp and whether an absolute `VK_DRIVER_FILES` path should be set.
- Launch a legally obtained homebrew test executable or other redistributable test fixture, never copyrighted commercial game content.
- Confirm where current shadPS4 writes saves, logs, settings, caches, trophies, screenshots, and per-game configuration under an isolated `HOME`.
- Capture a versioned CLI capability manifest. A changed or missing required CLI flag must disable install/update rather than silently guessing.

Exit gate:

- A checked-in probe fixture and test document prove the current stable core can initialize, write only inside the isolated state root, and terminate under supervision.

## Phase 1 — provider contracts and storage

Add a provider named `shadps4` behind a build-time experimental flag, without exposing it in production UI.

Planned C files:

```text
app/src-c/include/metalsharp_backend/shadps4.h
app/src-c/runtime/shadps4.c
app/src-c/tests/shadps4_release.json
app/src-c/tests/shadps4_bad_archive.zip
```

Add Rust parity models in `app/src-rust/src/emulators.rs`; Rust remains the contract oracle while C remains the packaged runtime.

Status states:

```text
unsupported_host
missing_runtime
runtime_ready
no_game_folders
ready
running
update_available
update_failed
```

`unsupported_host` includes a machine-readable reason such as `intel_mac`, `macos_too_old`, `rosetta_missing`, `insufficient_memory`, or `runtime_probe_failed`. CPU and memory requirements should be warnings unless upstream makes them hard requirements; architecture, OS deployment target, and Rosetta are hard gates.

Exit gate:

- C and Rust serialize identical provider, status, game, update, progress, and session contracts.
- No shadPS4 route is advertised when the experimental flag is off.

## Phase 2 — secure runtime installation and updates

Implement a stable-channel runtime manager by adapting the proven RPCS3 transaction model without assuming identical archive or signature properties.

Installation transaction:

1. Fetch official stable release metadata over HTTPS and cache it for 12 hours.
2. Select exactly one macOS SDL ZIP asset matching the release's documented name pattern.
3. Require a positive byte size and GitHub-provided `sha256:` digest.
4. Download to a unique `.part` file and reject truncation, overrun, redirects outside the approved GitHub asset hosts, or non-HTTPS final URLs.
5. Verify exact byte count and SHA-256 before extraction.
6. Preflight every ZIP entry; reject absolute paths, `..`, NULs, duplicate normalized paths, escaping links, devices, sockets, and unsupported entry types.
7. Extract into a same-volume staging directory, then repeat containment and symlink checks on disk.
8. Require the core executable, both Vulkan libraries, and ICD JSON. Parse the JSON and require its library path to resolve inside the staged version.
9. Require all shipped Mach-O files to be x86_64 and reject unexpected non-system dynamic-library references.
10. Read `LC_BUILD_VERSION` and reject a runtime newer than the host OS.
11. Because upstream currently ships unsigned binaries, preserve the verified original release digest in `source.json`, then ad-hoc sign each staged Mach-O locally and verify every signature with `codesign --verify --strict`.
12. Save the exact upstream GPL license and source URL for the selected tag beside the runtime.
13. Run `arch -x86_64 ./shadps4 --help` with an isolated `HOME`, bounded output, and a timeout. Require the expected capability tokens.
14. Wait for active shadPS4 sessions to exit before activation.
15. Atomically move staging into `versions/<tag>` and switch `current`, retaining the old target as `previous`.

Any failure leaves `current` unchanged and removes `.part` and staging artifacts. Rollback switches only the runtime pointer and never restores or rewinds user state.

Runtime removal deletes only runtime versions, pointers, downloads, and staging. It preserves `home`, `library.json`, sessions, logs, and all external roots.

Exit gate:

- Tests cover wrong digest, wrong size, ZIP traversal, duplicate paths, escaping symlinks, missing files, wrong architecture, unsupported minimum OS, invalid ICD JSON, failed local signing, failed CLI probe, active-session update blocking, interrupted activation, rollback, and state preservation.

## Phase 3 — owned-content onboarding and game discovery

### Game roots

- Accept only a user-selected existing directory.
- Canonicalize the root, reject filesystem roots and protected/system locations, and deduplicate by canonical identity.
- Scan with bounded depth, entry count, path length, and metadata size.
- Do not traverse directory symlinks.
- Recognize base game folders containing both `eboot.bin` and `sce_sys/param.sfo`.
- Recognize `CUSAxxxxx-patch` and `CUSAxxxxx-UPDATE` as update content associated with a base title, not duplicate cards.
- Defer `.zar` archives until bounded metadata reading can be implemented without extracting untrusted content.

Reuse the bounded SFO parser to read title, CUSA ID, application version, content ID, and category. Serve `sce_sys/icon0.png` as local card artwork after image decoding limits and path containment checks.

### Firmware modules and fonts

- Present these as optional compatibility files, not a firmware installation requirement.
- Let the user choose a directory dumped from their own console.
- Copy, never move, selected files into isolated state via staging and atomic replacement.
- Accept only the module names supported by the installed shadPS4 capability manifest; reject symlinks, oversized files, malformed binaries, duplicates, and unknown names by default.
- Import console-dumped `font` and `font2` directories with the same path and size protections.
- Never accept or extract an official PS4 update PUP.
- Never download modules, fonts, trophy keys, games, updates, or DLC.

Trophy-key onboarding is deferred. If later approved, it requires a separate threat model and secret-storage design; it must not be written to logs, API responses, or general configuration backups.

Exit gate:

- Fixtures prove base/update association, malformed SFO rejection, symlink containment, scan bounds, duplicate-root handling, artwork limits, atomic module import, and zero deletion of external content.

## Phase 4 — launch and process supervision

Launch the core directly, for example:

```text
<current>/shadps4 --fullscreen true --config-global -g <validated-eboot-path>
```

The exact arguments must come from the installed runtime's validated capability manifest, not a timeless hard-coded assumption.

For every launch:

- set the working directory to `current`;
- set isolated `HOME` and an absolute KosmicKrisp ICD path;
- pass arguments as an argv array without a shell;
- create a dedicated process group;
- write a per-launch log and atomic session record;
- store PID, process start time, executable identity, game ID, game path, runtime tag, and log path;
- validate PID ownership and executable identity before reporting, signaling, or recovering a session;
- terminate the process group with a bounded graceful interval before escalation;
- preserve and report exit status after backend restart;
- prevent simultaneous launches of the same game while allowing a future policy for distinct games.

Do not inject Wine, GPTK, D3DMetal, Steam bottle, or RPCS3 environment variables into shadPS4. It is a native macOS host process translated by Rosetta, with its own Vulkan stack.

Exit gate:

- Launch, stop, stale PID, PID reuse, backend restart, crash, concurrent launch, log containment, and child-process cleanup tests pass under ASAN and normal C test runs.

## Phase 5 — backend API and Electron boundary

Proposed read endpoints:

```text
GET /sharp-library/shadps4/status
GET /sharp-library/shadps4/games
GET /sharp-library/shadps4/cover?id=<id>
GET /sharp-library/shadps4/update/check
GET /sharp-library/shadps4/update/progress
```

Proposed mutations:

```text
POST /sharp-library/shadps4/scan
POST /sharp-library/shadps4/add-root
POST /sharp-library/shadps4/remove-root
POST /sharp-library/shadps4/import-modules
POST /sharp-library/shadps4/import-fonts
POST /sharp-library/shadps4/launch
POST /sharp-library/shadps4/stop
POST /sharp-library/shadps4/update/refresh
POST /sharp-library/shadps4/update/install
POST /sharp-library/shadps4/update/rollback
POST /sharp-library/shadps4/pin-current
POST /sharp-library/shadps4/unpin
POST /sharp-library/shadps4/skip-update
POST /sharp-library/shadps4/clear-skip
POST /sharp-library/shadps4/remove-runtime
```

Electron may expose only typed file/directory pickers and approved environment, log, and registered-root open operations. Main-process validation must independently enforce the shadPS4 environment and registered-root allowlist; renderer-provided paths are untrusted.

Exit gate:

- Contract tests reject traversal, unknown paths, method confusion, oversized bodies, malformed IDs, and renderer attempts to open or mutate paths outside approved roots.

## Phase 6 — Sharp Library experience

Only after Phases 0–5 pass, add a **shadPS4** header tab and advertise the provider from `/emulators`.

The first-run sequence is:

1. **Host readiness** — explain Apple Silicon, macOS, and Rosetta requirements.
2. **Install verified runtime** — show source, stable version, size, digest verification, and experimental status.
3. **Add game folder** — accept an already dumped game directory and clearly explain required `CUSAxxxxx` layout.
4. **Optional compatibility files** — import user-dumped modules and fonts without blocking games that do not require them.

Dashboard requirements:

- persistent **Experimental** badge;
- current runtime and update channel;
- architecture/translation and minimum-OS readiness;
- game, module, and active-session counts;
- clear stable update, pin, skip, rollback, and runtime-removal controls;
- explicit preservation language beside rollback and removal;
- cards with title, CUSA ID, version, artwork, running state, Play/Stop, Open Folder, and Open Log;
- empty, loading, unsupported, offline, update-failed, and no-games states that never masquerade as installable readiness;
- accurate upstream naming, branding, routes, and compatibility claims.

Exit gate:

- Keyboard navigation, focus states, reduced motion, screen-reader labels, narrow-window layout, loading-state flicker, and destructive-action confirmation are manually and automatically verified in the packaged app.

## Phase 7 — compatibility metadata and advanced features

These are follow-up features and do not block the first production integration:

- opt-in stable/nightly channels with independent pin and rollback history;
- cached upstream compatibility status keyed by CUSA ID, with source link and freshness timestamp;
- per-game configuration controls backed by shadPS4's supported config schema;
- update and DLC folder association;
- `.zar` discovery after a safe bounded reader exists;
- Big Picture mode after its process and child lifecycle are proven;
- controller profile management;
- a privacy-reviewed, explicit compatibility-report workflow;
- trophy-key support only after a separate security decision.

## Licensing and notices

Before production exposure:

- add shadPS4, SDL, Vulkan loader, KosmicKrisp/Mesa components, and all transitive runtime notices to `THIRD_PARTY_LICENSES`;
- preserve the exact upstream license and source revision beside each downloaded runtime;
- link users to corresponding source for the exact selected tag;
- document that MetalSharp is not affiliated with Sony Interactive Entertainment or the shadPS4 project;
- have the release process re-evaluate GPL obligations if MetalSharp ever bundles or redistributes the binary instead of downloading it on user request.

MetalSharp must not modify or patch shadPS4 code for the first integration. Local ad-hoc signing occurs only after the official asset digest is verified and is recorded in runtime provenance.

## Verification matrix

Every production PR must pass:

- `make -C app/src-c test`
- `make -C app/src-c asan-test`
- `cargo fmt --check`
- `cargo test`
- TypeScript, Vue/Vite, formatter, and C formatter checks
- corrupt/traversal update fixtures
- C/Rust JSON contract parity
- packaged application code-sign verification
- installed-backend route smoke tests
- installed `app.asar` inspection

Manual matrix:

- supported Apple Silicon + Rosetta + current macOS;
- Rosetta absent;
- Intel Mac rejection;
- host OS older than the asset deployment target;
- offline metadata with a valid cached release;
- first install, update, failed update, pin, skip, rollback, and runtime removal;
- empty root, valid game, malformed game, patch-only root, and symlinked root;
- module/font import, replacement failure, and preservation after runtime removal;
- launch, stop, crash, backend restart, app restart, and stale session recovery.

## Production definition of done

shadPS4 becomes a supported provider only when all of the following are true:

- The provider has accurate shadPS4 identity and experimental messaging.
- Host readiness fails closed using executable facts, not stale documentation.
- Official release size and digest are verified before extraction.
- ZIP extraction, Mach-O validation, local signing, activation, and rollback are atomic.
- Runtime and user state are isolated and independently removable.
- Game discovery is bounded and external content is never deleted.
- Launch and stop are shell-free, supervised, recoverable, and PID-safe.
- Sony content is neither downloaded nor bundled.
- C and Rust contracts agree.
- Backend, Electron, renderer, documentation, notices, and packaged artifacts are complete.
- Normal, ASAN, Rust, frontend, packaging, and installed-app tests pass.
- A prompt-to-artifact audit maps every requirement above to concrete evidence.

Until then, the repository may contain this roadmap and gated development code, but the production provider list and Sharp Library must remain RPCS3-only.
