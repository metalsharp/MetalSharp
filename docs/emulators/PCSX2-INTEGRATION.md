# PCSX2 integration

MetalSharp provides an isolated, managed environment for the official stable PCSX2 macOS app.

## What MetalSharp manages

- Official stable release discovery and 12-hour metadata caching.
- Exact asset size and SHA-256 verification.
- Safe `.tar.xz` preflight and shell-free extraction with `lsar` and `unar`.
- x86_64 architecture, macOS deployment target, contained dependencies, bundle identity, Developer ID team, hardened runtime, and notarization checks.
- Versioned, read-only activation with pin, skip, repair, rollback, and configuration backup.
- A private PCSX2 home at `~/.metalsharp/emulators/pcsx2/home`.
- User-owned BIOS validation and atomic import.
- Exact indexing of individually selected disc images, plus bounded discovery in explicitly selected game folders.
- Allowlisted PCSX2 controller-type and renderer settings written atomically to the isolated upstream configuration.
- Direct, shell-free launch and restart-safe process supervision.
- Preservation of BIOS, memory cards, saves, savestates, settings, controller profiles, covers, caches, screenshots, logs, and games during runtime updates and removal.

MetalSharp does not bundle PCSX2 and never downloads or uploads Sony BIOS files, games, disc images, licenses, or other console content.

## Host requirements

- macOS 11 or newer;
- Intel x86-64 with SSE4.1, or Apple Silicon with Rosetta 2;
- 8 GiB RAM recommended by upstream;
- game-specific compatibility and performance vary.

The currently managed stable app is x86_64. Apple Silicon launches use Rosetta explicitly. PCSX2 does not use Wine, GPTK, D3DMetal, Steam bottles, RPCS3 state, or shadPS4 state.

## Setup

1. Open **Sharp Library → PCSX2**.
2. Install the verified official stable runtime.
3. Follow PCSX2's [official BIOS dumping guide](https://pcsx2.net/docs/setup/bios/) and import a BIOS dumped from a PlayStation 2 you own.
4. Expand **PCSX2 Setup** in MetalSharp and select the virtual controller type for ports 1 and 2 and the renderer. These settings are saved directly to PCSX2 without opening its application.
5. Add an owned disc image or a dedicated game folder. The [official disc-dumping guide](https://pcsx2.net/docs/setup/discs/) explains supported dumping methods.

Supported library files are ISO, BIN, IMG, MDF, GZ, CSO, ZSO, CHD, and homebrew ELF. CUE, TOC, and CDR sidecars are not launchable PCSX2 library entries.

Selecting one file indexes only that file; MetalSharp does not scan the file's parent directory. Selecting a folder opts that folder into bounded recursive discovery. Removing a location removes only its reference and never deletes or rewrites external content.

PCSX2 Setup exposes the exact upstream controller values DualShock 2, Guitar, JogCon, NeGcon, and Pop'n Music for both emulated ports. Renderer choices are Automatic, Metal, OpenGL, Vulkan, and Software—the methods present in the verified macOS runtime. Each change is allowlisted, written atomically, and reloaded whenever MetalSharp opens the page. Settings changes are blocked while PCSX2 or a runtime transaction is active to prevent configuration races. Advanced input mapping and other expert options remain available through **Open PCSX2**, but they are not required for these baseline choices.

## Data layout

```text
~/.metalsharp/emulators/pcsx2/
├── current -> versions/<tag>
├── previous -> versions/<tag>
├── versions/<tag>/
│   ├── PCSX2.app/
│   ├── LICENSE
│   ├── THIRD_PARTY_LICENSES.html
│   ├── source.json
│   └── capabilities.json
├── home/Library/Application Support/PCSX2/
├── downloads/
├── staging/
├── sessions/
├── logs/
├── backups/
├── environment.json
├── update-policy.json
└── library.json
```

Runtime removal deletes only downloaded runtime versions, activation pointers, downloads, and staging. The isolated home, backups, policies, library references, logs, sessions, and external games remain.

## Updates and rollback

MetalSharp disables PCSX2's startup updater in the isolated configuration. This keeps runtime changes inside the verified transaction:

1. download to a unique partial file;
2. verify size and SHA-256;
3. reject unsafe archive entries;
4. extract into same-volume staging;
5. verify app identity, every Mach-O, dependencies, signature, hardened runtime, notarization, and CLI;
6. back up configuration and controller profiles;
7. freeze and atomically commit the new version;
8. activate it while retaining the previous version.

A failed transaction leaves the active version unchanged. Rollback switches only the runtime. It does not rewind memory cards or user data. PCSX2 savestates can be version-sensitive, so the UI warns before rollback.

## BIOS privacy and validation

BIOS import accepts a regular file or bounded dump directory. The main ROM must be 4–8 MiB and pass PCSX2-compatible ROMDIR/ROMVER validation. Known companion files are accepted from a selected dump directory.

Imports are copied through private staging. The previous valid BIOS directory is restored if replacement fails. API responses expose only the detected description and region. BIOS contents, paths, and hashes are not uploaded or included in general diagnostics.

## Game metadata and artwork

MetalSharp always provides a sanitized filename title. For uncompressed images it may read at most the first 32 MiB to locate a normal PS2 serial such as `SLUS-12345`. Compressed formats retain filename metadata rather than being decompressed during a scan.

PCSX2-local covers are used only when a size-limited regular image matches the serial or MetalSharp game ID. Baseline scanning performs no artwork scraping and makes no compatibility-site requests.

## API

Read endpoints:

```text
GET /emulators
GET /sharp-library/pcsx2/status
GET /sharp-library/pcsx2/games
GET /sharp-library/pcsx2/settings
GET /sharp-library/pcsx2/cover?id=<id>
GET /sharp-library/pcsx2/update/check
GET /sharp-library/pcsx2/update/progress
```

Mutation endpoints:

```text
POST /sharp-library/pcsx2/initialize
POST /sharp-library/pcsx2/configure
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

No endpoint accepts an arbitrary executable, URL, command, shell fragment, PCSX2 configuration key, or file-open target. The configure endpoint accepts only the documented controller and renderer identifiers and maps them to audited upstream INI sections and values.

## Trust boundary

Electron exposes only a BIOS picker, a game-file-or-folder picker, the two official PCSX2 guides, and contained path reveals. Every reveal is resolved again in the main process and must remain inside the PCSX2 environment or a registered game root. Revealing a file selects it in Finder; it does not open or execute the content.

The complete release, host, CLI, isolation, BIOS, discovery, update, and process contract is in [PCSX2-UPSTREAM-CONTRACT.md](PCSX2-UPSTREAM-CONTRACT.md). Historical phase gates and research evidence remain in [PCSX2-INTEGRATION-ROADMAP.md](PCSX2-INTEGRATION-ROADMAP.md).
