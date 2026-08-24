# PCSX2 integration roadmap

Status: **production phases 0–7 complete; Phase 8 remains intentionally deferred**

Implemented on 2026-08-24. The active provider contract is documented in [PCSX2-INTEGRATION.md](PCSX2-INTEGRATION.md) and [PCSX2-UPSTREAM-CONTRACT.md](PCSX2-UPSTREAM-CONTRACT.md). Phase 8 lists optional future work which this roadmap explicitly defines as non-blocking; none of those features is advertised as available.

MetalSharp will identify this provider by its accurate upstream name: **PCSX2**.

This roadmap adds an isolated, MetalSharp-managed PlayStation 2 environment without downloading or bundling Sony BIOS files, games, disc images, licenses, or other copyrighted console content. A BIOS dumped from a console the user owns is required by PCSX2 and remains entirely user-supplied.

## Research snapshot

Validated against upstream on 2026-08-24:

- Core repository: <https://github.com/PCSX2/pcsx2>
- Core revision inspected: `3e29183a37e74cbc8c17bda8afb63c2d9bc6fd14`
- Official documentation repository: <https://github.com/PCSX2/pcsx2-net-www>
- Documentation revision inspected: `24ba3e41793165062c1ddcb434460033471f3f8c`
- Stable releases: <https://github.com/PCSX2/pcsx2/releases/latest>
- Downloads: <https://pcsx2.net/downloads/>
- System requirements: <https://pcsx2.net/docs/setup/requirements/>
- BIOS dumping guide: <https://pcsx2.net/docs/setup/bios/>
- Disc dumping guide: <https://pcsx2.net/docs/setup/discs/>
- Command-line reference: <https://pcsx2.net/docs/advanced/cli/>
- Compatibility database: <https://pcsx2.net/compat/>
- License: GPL-3.0-or-later

The official stable release inspected was `v2.6.3`. Its macOS asset is:

```text
pcsx2-v2.6.3-macos-Qt.tar.xz
size:   28,960,388 bytes
sha256: cb7b9e6330f1abf0cf92c94065f7eb983d0fa8affcfe6b0ccb9c2a4ebf067f1a
```

The release API publishes both the exact size and `sha256:` digest. The archive contains one versioned app bundle, `PCSX2-v2.6.3.app`, whose observed properties were:

- bundle identifier `net.pcsx2.pcsx2`;
- executable `Contents/MacOS/PCSX2`;
- x86_64-only Mach-O architecture;
- minimum deployment target macOS 11.0;
- Developer ID signature from team `PTMR35SWS3`;
- hardened runtime and a stapled notarization ticket;
- bundled Qt, SDL, Metal shaders, MoltenVK, FFmpeg, GameDB, patches, GPL notice, and third-party notices;
- no archive or bundle symlinks in the inspected stable asset.

The newest prerelease observed was `v2.7.524`, but stable is the only planned initial channel. Official documentation says PCSX2 runs through Rosetta 2 on Apple Silicon. Upstream source also requires x86-64/SSE4.1 on Intel and documents macOS 11 and 8 GB RAM as minimums.

### Important CLI compatibility finding

The current documentation and development branch list `-datapath <path>`, but stable `v2.6.3` does **not** expose or accept it. Upstream added that flag in commit `bd486f172970bba3c3fd1b93ffd36b426129ce5f`, first contained by `v2.7.296`.

Stable `v2.6.3` does expose `-help`, `-version`, `-batch`, `-nogui`, `-portable`, `-logfile`, `-bios`, `-fastboot`, `-slowboot`, `-fullscreen`, `-bigpicture`, `-testconfig`, `-setupwizard`, and `-- <boot filename>`.

An isolated non-game `-testconfig` probe confirmed that `v2.6.3` uses:

```text
$HOME/Library/Application Support/PCSX2/
```

and creates `bios`, `cache`, `cheats`, `covers`, `gamesettings`, `inis`, `inputprofiles`, `logs`, `memcards`, `patches`, `resources`, `snaps`, `sstates`, `textures`, and `videos`. MetalSharp must therefore isolate stable releases with `HOME`; it may additionally use `-datapath` only when the installed runtime's validated capability manifest proves the flag exists. `-portable` is unsuitable because it mixes mutable user data into the versioned app bundle.

## Product decisions

1. **Use the official PCSX2 Qt app.** MetalSharp launches the signed upstream bundle directly; it does not fork, patch, embed, or re-sign PCSX2.
2. **Stable channel first.** Nightly releases remain unavailable until stable installation, state migration, update, and rollback are proven.
3. **Support Intel and Apple Silicon hosts.** The current app is x86_64. Intel requires SSE4.1; Apple Silicon requires a working Rosetta 2 translation probe.
4. **Isolate data from runtime.** Set an isolated `HOME` for every PCSX2 command. Use `-datapath` only when capability-probed. Never use `-portable`.
5. **Let PCSX2 own emulator-specific configuration.** MetalSharp owns installation, library, preservation, and process supervision; the upstream UI owns controller mapping, renderer tuning, memory-card management, per-game settings, and its setup wizard.
6. **MetalSharp manages runtime updates.** Disable PCSX2's startup update check in isolated configuration after a versioned schema probe, and verify the app signature before every launch. Upstream self-update must not mutate `current` outside MetalSharp's transaction.
7. **BIOS acquisition is user-directed.** MetalSharp links the official dumping guide and imports a user-selected dump. It never downloads, bundles, suggests search terms for, or transmits a Sony BIOS.
8. **Games remain external references.** MetalSharp scans user-selected roots and launches supported files in place. Removing a root never deletes a disc image or game directory.
9. **Do not build a disc-ripping feature.** Link the official disc-dumping guide. Disk Utility, `dd`, `cdrdao`, and optical-drive access remain outside MetalSharp.
10. **Do not overclaim metadata.** Uncompressed disc images may receive serial/title metadata from a bounded parser. Compressed formats fall back to a filename title unless a verified upstream contract supplies metadata.
11. **No account or secret handling initially.** RetroAchievements login, tokens, and network features remain inside PCSX2 and are never returned by MetalSharp APIs or copied into general logs.
12. **No dormant provider.** Do not advertise PCSX2 from `/emulators` or add a production tab until runtime, BIOS, discovery, launch, stop, preservation, and packaging gates pass.

## Target filesystem model

```text
~/.metalsharp/emulators/pcsx2/
├── current -> versions/<release-tag>
├── previous -> versions/<release-tag>
├── versions/<release-tag>/
│   ├── PCSX2-<release-tag>.app/
│   ├── LICENSE
│   ├── THIRD_PARTY_LICENSES.html
│   ├── source.json
│   └── capabilities.json
├── home/
│   └── Library/Application Support/PCSX2/
│       ├── bios/
│       ├── cache/
│       ├── cheats/
│       ├── covers/
│       ├── gamesettings/
│       ├── inis/
│       ├── inputprofiles/
│       ├── logs/
│       ├── memcards/
│       ├── patches/
│       ├── resources/
│       ├── snaps/
│       ├── sstates/
│       ├── textures/
│       └── videos/
├── downloads/
├── staging/
├── sessions/
├── logs/
├── backups/
├── environment.json
└── library.json
```

The version directory is immutable after activation. Runtime removal deletes `versions`, pointers, downloads, and staging only. It preserves the entire isolated home, BIOS, memory cards, save states, settings, controller profiles, caches, covers, screenshots, patches, cheats, logs, sessions, and external game roots.

Savestates are not guaranteed to be compatible between PCSX2 versions. Rollback must warn about this and may preserve a pre-update configuration backup, but it must never silently rewind memory cards, game saves, or other user state.

## Phase 0 — freeze the upstream contract

Create `docs/emulators/PCSX2-UPSTREAM-CONTRACT.md` and reproducible non-shipping probes before provider code.

Record and test:

- stable and prerelease tag semantics;
- exact macOS asset naming and release API fields;
- approved HTTPS redirect hosts;
- archive topology and entry types;
- bundle identifier, executable, architecture, deployment target, signing team, hardened-runtime state, and notarization;
- every Mach-O dependency and containment inside the bundle or macOS system paths;
- actual `-help` and `-version` output for the selected stable;
- isolated `HOME` behavior and every write location from `-testconfig`, setup, UI, and a redistributable homebrew ELF fixture;
- `-datapath` presence or absence by release, never by documentation alone;
- PCSX2 INI string-list serialization for game roots and the exact updater key;
- setup-wizard completion behavior and BIOS selection behavior;
- clean launch/exit semantics for `-nogui -batch -- <file>`;
- app child-process behavior and logs;
- whether a runtime can safely read state first created by the next supported stable.

Save a versioned capability fixture containing only observed features, for example:

```json
{
  "schemaVersion": 1,
  "provider": "pcsx2",
  "runtimeTag": "v2.6.3",
  "architecture": "x86_64",
  "minimumMacOS": "11.0",
  "bundleIdentifier": "net.pcsx2.pcsx2",
  "teamIdentifier": "PTMR35SWS3",
  "cli": ["-batch", "-nogui", "-logfile", "-testconfig", "--"],
  "dataIsolation": "home",
  "dataPathFlag": false
}
```

Exit gate:

- Tests prove the stable app initializes and runs a redistributable fixture while writing only inside the isolated home and explicit MetalSharp session/log directories.
- Any required CLI, signature, architecture, or isolation change fails closed.

## Phase 1 — provider contracts and host readiness

Planned C files:

```text
app/src-c/include/metalsharp_backend/pcsx2.h
app/src-c/runtime/pcsx2.c
app/src-c/tests/pcsx2_release.json
app/src-c/tests/pcsx2_bad_archive.tar.xz
```

Add equivalent Rust models in `app/src-rust/src/emulators.rs`. C remains the packaged runtime; Rust remains the contract oracle.

Status states:

```text
unsupported_host
missing_runtime
initializing
setup_required
missing_bios
no_game_folders
ready
running
update_available
update_failed
```

Machine-readable unsupported reasons include `unsupported_architecture`, `macos_too_old`, `rosetta_missing`, `sse41_missing`, `signature_invalid`, and `runtime_probe_failed`. Core count and RAM are warnings unless a real runtime probe proves they are hard blockers.

Readiness rules:

- reject non-Intel/non-Apple-Silicon architectures;
- on Intel, require SSE4.1 and the app's minimum macOS;
- on Apple Silicon, require macOS compatibility and a successful bounded x86_64 translation probe;
- derive the final OS gate from `LC_BUILD_VERSION` and `Info.plist`, using the stricter value;
- report Metal support and memory/core guidance without pretending a benchmark predicts per-game performance.

Exit gate:

- C and Rust serialize identical provider, status, game, BIOS, update, progress, and session contracts.
- The provider remains hidden when its build-time experimental flag is off.

## Phase 2 — secure runtime installation and updates

Use the proven atomic emulator transaction model, adapted for a signed `.tar.xz` app bundle.

Installation transaction:

1. Fetch `PCSX2/pcsx2` release metadata over HTTPS and cache stable metadata for 12 hours.
2. Ignore drafts and prereleases on the stable channel.
3. Select exactly one asset matching `pcsx2-<tag>-macos-Qt.tar.xz`.
4. Require a positive asset size and GitHub `sha256:` digest.
5. Download to a unique `.part` file; reject truncation, overrun, non-HTTPS URLs, and redirects outside approved GitHub release hosts.
6. Verify exact byte count and SHA-256 before archive inspection.
7. Preflight the archive with bounded structured metadata from `lsar`; reject absolute paths, `..`, NULs, duplicate normalized paths, links, devices, sockets, multiple top-level bundles, and unsupported types.
8. Extract shell-free with `unar` into a same-volume unique staging directory.
9. Rewalk staging without following symlinks and require exactly `PCSX2-<tag>.app` with no escaping, links, hard links, or unexpected siblings.
10. Require the bundle identifier, executable, version, x86_64 architecture, minimum OS, and resources needed for Qt/Metal startup.
11. Reject non-system dynamic-library references that do not resolve inside the staged app.
12. Verify `codesign --verify --deep --strict`, the pinned signing-team contract, hardened runtime, and Gatekeeper/notarization assessment. Do not ad-hoc sign or modify the app.
13. Run bounded `-version`, `-help`, and `-testconfig` probes with an empty isolated home. Build `capabilities.json` from observed output.
14. Preserve the exact release URL, asset ID, size, digest, signature metadata, source tag, GPL notice, and bundled third-party notices.
15. Wait for all managed PCSX2 sessions and the upstream configuration UI to exit.
16. Atomically activate `versions/<tag>` and retain the old target as `previous`.

A failure leaves `current` unchanged and removes partial/staging artifacts. Startup recovery removes abandoned `.part` and staging paths only after proving they belong to the PCSX2 environment and are not active.

Before launching any activated version, revalidate the version pointer, app identity, executable identity, and deep signature. If upstream self-update or local mutation changed the bundle, mark the runtime damaged and offer repair rather than running it.

Exit gate:

- Tests cover wrong size/digest, malformed XZ, traversal, duplicate entries, links, special files, multiple bundles, wrong bundle ID/version/architecture, unsupported deployment target, escaped dylibs, signature/team/notarization failure, missing resources, CLI drift, active-session handoff, interrupted activation, rollback, repair, and state preservation.

## Phase 3 — isolated initialization and BIOS onboarding

### Isolated initialization

- Precreate `home/Library/Application Support` with private permissions before invoking PCSX2.
- Run `-testconfig` under the isolated `HOME` to initialize the selected stable.
- If the capability manifest proves `-datapath`, pass the exact isolated PCSX2 data path as an additional defense; otherwise omit it.
- Never pass `-portable` or let mutable state enter the signed app bundle.
- Disable `[AutoUpdater] CheckAtStartup` only through a version-tested, atomic INI mutation that preserves unknown sections, comments where possible, and unrelated user settings.
- Treat `SetupWizardIncomplete` as a real `setup_required` state. Provide **Open PCSX2 Setup** rather than guessing controller, language, renderer, or audio preferences.
- Never edit PCSX2 configuration while a PCSX2 process is active.

### BIOS import

- Link only the official PCSX2 BIOS dumping guide.
- Accept a user-selected regular file or dump directory from a console they own.
- Canonicalize the source, reject symlinks and protected/system locations, and enforce path, file-count, and byte limits.
- Require at least one valid main ROM image between 4 MiB and 8 MiB, matching the bounded ROMDIR/ROMVER checks used by upstream `BiosTools`.
- Accept only known companion dump extensions and names after research freezes the contract; reject executables, archives, unknown files, duplicates, and device files.
- Copy, never move, through private staging and atomic replacement into isolated `bios/`.
- Do not log BIOS contents, include them in API responses, upload hashes, or place them in general diagnostics. A local-only integrity record may be stored with restrictive permissions.
- Preserve the previous valid BIOS set if import fails.

MetalSharp never downloads `biosdrain`, BIOS databases, BIOS files, or console firmware as part of provider setup. It may open the upstream instructions in the user's browser.

Exit gate:

- Tests cover valid regional fixtures using synthetic/non-copyrighted structures, malformed ROMDIR/ROMVER, wrong size, symlinks, duplicate names, oversized companions, partial copy failure, atomic replacement, setup detection, updater disablement, and preservation after runtime removal.

## Phase 4 — owned-game discovery and metadata

### Root handling

- Accept only user-selected existing directories.
- Canonicalize, reject filesystem roots and protected/system directories, and deduplicate by canonical identity.
- Keep MetalSharp roots in `library.json`; synchronize PCSX2 `GameList` paths only after Phase 0 proves the INI schema and only while PCSX2 is stopped.
- Removing a root updates manifests/configuration only. It never deletes, renames, converts, patches, or writes into external game content.

### Discovery

Scan with bounded depth, entry count, elapsed time, path length, and metadata reads. Do not traverse directory symlinks. Recognize the extensions supported by the inspected upstream source:

```text
.iso .bin .img .mdf .gz .cso .zso .chd .elf
```

`.cue`, `.toc`, and `.cdr` are not launchable even though they may accompany a legitimate dump; the UI must explain the official limitation rather than silently listing them. GS dumps, block dumps, save states, and arbitrary executables are not normal library games.

For every entry:

- use canonical path plus provider namespace for stable local identity;
- record file size, modification identity, format, and root without hashing multi-gigabyte images during routine scans;
- use a sanitized filename as the guaranteed title fallback;
- parse ISO9660/UDF and `SYSTEM.CNF` only through bounded sector reads for uncompressed images;
- normalize any discovered serial such as `SLUS-xxxxx` and optionally resolve title/region from the exact runtime's `GameIndex.yaml`;
- treat all on-disc strings and GameDB values as untrusted display text;
- do not implement custom decompression for CHD/CSO/ZSO/GZ merely to obtain metadata; launch them through PCSX2 and retain fallback metadata;
- detect multi-disc siblings without merging paths or saves incorrectly;
- serve only decoded, size-limited local cover files from the isolated covers directory or explicitly user-selected artwork.

Do not scrape box art or compatibility websites during baseline scanning.

Exit gate:

- Fixtures cover all recognized extensions, unsupported sidecars, bounded ISO metadata, malformed sectors, compressed fallback, duplicate roots, hard links, symlinks, sparse/huge files, scan cancellation, multi-disc naming, artwork limits, and zero external-content deletion.

## Phase 5 — launch, UI access, and process supervision

Direct game launch uses the exact validated capability manifest. The baseline stable command is conceptually:

```text
<current-app>/Contents/MacOS/PCSX2 \
  -nogui -batch -fullscreen \
  -logfile <contained-log-path> \
  -- <validated-game-path>
```

On Apple Silicon, launch through a fixed argv equivalent of `/usr/bin/arch -x86_64`; never use a shell. On Intel, execute PCSX2 directly.

For every process:

- require a valid imported BIOS and completed required setup;
- require the selected game to be a current scanned entry under a registered root;
- set isolated `HOME` and, when supported, `-datapath`;
- set the app bundle's directory as working directory only if the upstream probe requires it;
- pass no Wine, GPTK, D3DMetal, Steam-bottle, RPCS3, or shadPS4 environment variables;
- create a dedicated process group;
- capture stdout/stderr separately from PCSX2's `-logfile` output;
- atomically store PID, process start time, executable identity, game ID/path, runtime tag, architecture mode, and log paths;
- validate PID, start time, executable, and environment ownership before status, recovery, or signaling;
- terminate the process group gracefully with a bounded timeout before escalation;
- preserve exit code/signal and recover stale/crashed sessions after backend restart;
- reject duplicate launches and block runtime/configuration mutations while any PCSX2 process is active.

Provide **Open PCSX2** and **Open Setup** actions for controller mapping, renderer settings, memory cards, per-game settings, and diagnostics. These UI processes use the same isolated home and are supervised separately from game sessions. MetalSharp must not race PCSX2 for INI or game-list cache writes.

Exit gate:

- Normal and ASAN tests cover launch/stop, missing BIOS, setup incomplete, path replacement, stale PID, PID reuse, app/backend restart, crash, duplicate launch, UI/game exclusion, logs, process-group cleanup, and runtime-update blocking.

## Phase 6 — backend API and Electron boundary

Proposed read endpoints:

```text
GET /sharp-library/pcsx2/status
GET /sharp-library/pcsx2/games
GET /sharp-library/pcsx2/cover?id=<id>
GET /sharp-library/pcsx2/update/check
GET /sharp-library/pcsx2/update/progress
```

Proposed mutations:

```text
POST /sharp-library/pcsx2/initialize
POST /sharp-library/pcsx2/import-bios
POST /sharp-library/pcsx2/scan
POST /sharp-library/pcsx2/add-root
POST /sharp-library/pcsx2/remove-root
POST /sharp-library/pcsx2/launch
POST /sharp-library/pcsx2/stop
POST /sharp-library/pcsx2/open-ui
POST /sharp-library/pcsx2/open-setup
POST /sharp-library/pcsx2/update/refresh
POST /sharp-library/pcsx2/update/install
POST /sharp-library/pcsx2/update/rollback
POST /sharp-library/pcsx2/pin-current
POST /sharp-library/pcsx2/unpin
POST /sharp-library/pcsx2/skip-update
POST /sharp-library/pcsx2/clear-skip
POST /sharp-library/pcsx2/remove-runtime
```

No endpoint accepts an arbitrary executable, BIOS URL, download URL, shell fragment, configuration key, or open-path target.

Electron may expose only typed file/directory pickers plus approved opens for the PCSX2 environment, contained logs, official documentation URLs, and registered roots. The main process independently canonicalizes and validates every path; renderer-provided paths and IDs are untrusted.

Exit gate:

- Contract tests reject traversal, symlink swaps, unknown roots, malformed IDs, method confusion, oversized bodies, arbitrary URLs, arbitrary configuration writes, and path-open attempts outside explicit allowlists.

## Phase 7 — Sharp Library experience

Only after Phases 0–6 pass, advertise `pcsx2` from `/emulators` and add a **PCSX2** header tab.

First-run flow:

1. **Host readiness** — show macOS, Intel/Rosetta, CPU, and memory facts without performance guarantees.
2. **Install verified PCSX2** — show official source, stable version, size, digest, Developer ID, and notarization result.
3. **Import your PS2 BIOS** — explain ownership, link the official dump guide, and select a user dump.
4. **Finish PCSX2 setup** — open the isolated upstream wizard for controller, renderer, language, and audio choices.
5. **Add game folder** — explain supported dump formats and link the official disc-dumping guide.

Dashboard requirements:

- current runtime, stable channel, signature, architecture/translation, and minimum-OS status;
- BIOS readiness and region/description without exposing BIOS contents or paths unnecessarily;
- game, root, and active-session counts;
- install, repair, check, pin, skip, rollback, and runtime-removal controls;
- explicit preservation language beside update, rollback, root removal, and runtime removal;
- **Open PCSX2**, **Open Setup**, **BIOS Guide**, and **Disc Dumping Guide** actions;
- cards with safe title fallback, serial/region when known, format, file size, local cover, running state, Play/Stop, Open Folder, and Open Log;
- clear missing-Rosetta, missing-runtime, setup-required, missing-BIOS, no-roots, empty-library, offline, corrupt-runtime, update-failed, and running states;
- warnings that compatibility and performance vary by game;
- keyboard navigation, visible focus, screen-reader labels, reduced motion, narrow-window layouts, and confirmed destructive actions.

Exit gate:

- Automated frontend checks and manual packaged-app inspection cover every state at desktop, narrow, and mobile-like widths with no loading-state flicker or false readiness.

## Phase 8 — deferred advanced features

These do not block the first safe production provider:

- opt-in nightly channel with separate pin/rollback history and stronger state-migration warnings;
- cached official compatibility metadata keyed by serial/CRC, with source and freshness timestamp;
- PCSX2 Big Picture mode after UI-process lifecycle testing;
- per-game setting controls only through a versioned upstream schema;
- memory-card import/export UI after a separate irreversible-action review;
- save-state actions with strict runtime-version compatibility checks;
- controller-profile management after schema and device testing;
- local cover acquisition with an explicit source/privacy policy;
- PINE IPC only after authentication, port ownership, and local attack-surface review;
- RetroAchievements links and status only after a privacy review—never credential handling;
- PlayStation 1 support as a distinct product decision, not an accidental consequence of PCSX2 accepting a file;
- direct optical-disc launch or dumping only after a separate permission and data-loss threat model.

## Licensing and notices

Before production exposure:

- add PCSX2 and every redistributed/downloaded runtime component required by policy to `THIRD_PARTY_LICENSES`;
- preserve the exact GPL and bundled `ThirdPartyLicenses.html` beside each runtime;
- save corresponding-source URLs for the exact selected tag;
- link the PCSX2 trademark/copyright and anti-piracy policy;
- state that MetalSharp is not affiliated with Sony Interactive Entertainment or the PCSX2 project;
- do not use PCSX2 artwork or branding beyond what its license and trademark policy allow;
- re-evaluate GPL distribution obligations if MetalSharp ever bundles PCSX2 instead of downloading it on user request.

MetalSharp must not patch PCSX2 in the initial integration. The upstream Developer ID signature and notarization ticket are part of the trust contract and must remain intact.

## Verification matrix

Every production PR must pass:

- `make -C app/src-c test`
- `make -C app/src-c asan-test`
- `cargo fmt --check`
- `cargo test`
- TypeScript, Vue/Vite, Biome/formatter, and C formatter checks
- corrupt/traversal/link archive fixtures
- C/Rust JSON contract parity
- packaged app code-sign verification
- installed-backend route smoke tests
- installed `app.asar` inspection
- a repository and packaged-artifact audit confirming no Sony BIOS or game content

Manual matrix:

- Intel + SSE4.1 and Apple Silicon + Rosetta;
- Rosetta absent and unsupported macOS;
- low-memory/core warning behavior;
- offline metadata with valid cache;
- first install, repair, update, failed update, pin, skip, rollback, and runtime removal;
- setup incomplete/completed and updater disabled;
- valid, invalid, partial, duplicate, and symlinked BIOS imports;
- empty root and every supported/unsupported game format;
- compressed metadata fallback and multi-disc entries;
- launch, stop, crash, backend/app restart, stale session, and UI/game exclusion;
- preservation of BIOS, memory cards, states, settings, profiles, covers, caches, logs, and external games across update/rollback/removal.

## Production definition of done

PCSX2 becomes a production provider only when all of the following are true:

- Identity, source, stable channel, architecture, and host requirements are accurate.
- Release size/digest, archive safety, bundle identity, Developer ID signature, notarization, and CLI capabilities fail closed.
- Runtime and all mutable PCSX2 state are isolated and independently removable.
- Upstream self-update cannot bypass MetalSharp's atomic runtime manager.
- BIOS onboarding accepts only validated user-supplied dumps and MetalSharp never acquires Sony content.
- Game discovery is bounded, direct launches are shell-free, and external content is never deleted.
- Process supervision is PID-safe, restart-safe, and cleans child processes.
- C and Rust contracts agree.
- Backend, Electron, renderer, documentation, notices, and packaged artifacts are complete.
- Normal, ASAN, Rust, frontend, packaging, and installed-app tests pass.
- A prompt-to-artifact audit maps every requirement above to concrete evidence.

All production gates above now pass. PCSX2 is exposed as a managed stable provider. Deferred Phase 8 features remain unavailable until their separate security, privacy, compatibility, and state-migration gates are completed.
