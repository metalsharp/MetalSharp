# PCSX2 Managed Environment
**Updated:** 2026-09-22

MetalSharp runs the official stable PCSX2 macOS app in an isolated managed environment. PCSX2 is GPL-3.0-or-later; BIOS files are dumped by the user from a console they own, following [the official guide](https://pcsx2.net/docs/setup/bios/).

## Setup

1. Open **Sharp Library → PCSX2** and install the verified official stable runtime.
2. Import a BIOS dumped from a PlayStation 2 you own.
3. Expand **PCSX2 Setup** and pick the virtual controller type for ports 1 and 2 and the renderer.
4. Add an owned disc image or a dedicated game folder (disc-dumping guide: <https://pcsx2.net/docs/setup/discs/>).

Supported library files: ISO, BIN, IMG, MDF, GZ, CSO, ZSO, CHD, and homebrew ELF. Selecting one file indexes only that file; selecting a folder opts that folder into bounded recursive discovery. Removing a location removes only its reference.

PCSX2 Setup writes the exact upstream controller values (DualShock 2, Guitar, JogCon, NeGcon, Pop'n Music) and renderer choices (Automatic, Metal, OpenGL, Vulkan, Software) atomically to the isolated upstream configuration. Changes are blocked while PCSX2 or a runtime transaction is active.

## Runtime Contract

MetalSharp reads <https://api.github.com/repos/PCSX2/pcsx2/releases/latest> and accepts one non-draft, non-prerelease release with a single `pcsx2-<tag>-macos-Qt.tar.xz` asset carrying a positive byte size and `sha256:` digest. The inspected baseline is `v2.6.3` (28,960,388 bytes, SHA-256 `cb7b9e63…bf067f1a`, source rev `3e29183a`).

After extraction the app must pass: bundle id `net.pcsx2.pcsx2`, executable `Contents/MacOS/PCSX2`, every Mach-O x86_64, deployment target ≤ host, contained `@rpath`/`@loader_path`/`@executable_path` dependencies, Developer ID team `PTMR35SWS3`, hardened runtime, `codesign --verify --deep --strict`, and Gatekeeper notarization. Upstream signatures are preserved. Archive preflight rejects links, traversal, control characters, duplicates, a second top-level entry, more than 20,000 entries, or more than 2 GiB of declared output.

Host requirements: macOS 11+, Intel x86-64 with SSE4.1 or Apple Silicon with a bounded Rosetta probe, 8 GiB RAM recommended. The stable CLI (`-version`, `-help`, `-testconfig`) is probed before acceptance; required options `-batch -nogui -logfile -testconfig -setupwizard --` are verified. `-datapath` support is detected from the installed runtime's `-help` output and recorded in `capabilities.json`.

Game launches use fixed argv, no shell:

```text
[/usr/bin/arch -x86_64] PCSX2 -nogui -batch -fullscreen -logfile <contained-log> -- <indexed-game>
```

## Data Layout

```text
~/.metalsharp/emulators/pcsx2/
├── current -> versions/<tag>
├── previous -> versions/<tag>
├── versions/<tag>/          # read-only: PCSX2.app, LICENSE, manifests
├── home/                    # isolated HOME; upstream data under Library/Application Support/PCSX2/
├── downloads/  staging/  sessions/  logs/  backups/
├── environment.json  update-policy.json  library.json
```

Runtime removal deletes versions, pointers, downloads, and staging; the isolated home, backups, library references, logs, sessions, and external games remain.

## Updates and Rollback

PCSX2's startup updater is disabled in the isolated configuration; runtime changes happen through the verified transaction: download → size/SHA-256 verify → archive preflight → same-volume staging extract → app identity/signature/notarization/CLI verify → configuration backup → atomic freeze/commit → activation with `previous` retained. A failed transaction leaves the active version unchanged. Rollback switches only the runtime; savestates can be version-sensitive and the UI warns first.

String-list edits touch only exact managed `[GameList] RecursivePaths` entries; unknown INI sections and unrelated values are preserved.

## BIOS Handling

The picker accepts `.bin` files (4–8 MiB, ROMDIR/ROMVER validation, recognized region). Imports copy through private staging and atomically replace the isolated BIOS directory, restoring the prior valid set on failure. API responses expose description and region only.

## Game Metadata

A bounded 32 MiB read may recover a normalized PS2 serial (e.g. `SLUS-12345`) from uncompressed images; otherwise the sanitized filename is the title. Compressed formats stay filename-based. PCSX2-local covers are used when a size-limited image matches the serial; baseline scanning makes no artwork or compatibility-site requests. Discovery bounds: 32 locations, depth 8, 20,000 entries, 512 displayed games.

## Process Supervision

One managed process at a time. Each launch has an isolated environment, fixed working directory, dedicated process group, private logs, and an atomically persisted session record with PID-reuse checks. Launch revalidates the upstream signature every time; mutations fail while PCSX2 is active.

## API

```text
GET  /emulators, /sharp-library/pcsx2/{status,games,settings,cover,update/check,update/progress}
POST /sharp-library/pcsx2/{initialize,configure,import-bios,scan,add-root,remove-root,
     launch,stop,open-ui,open-setup,update/refresh,update/install,update/rollback,
     pin-current,unpin,skip-update,clear-skip,remove-runtime}
```

Endpoints accept only documented identifiers; every path reveal resolves in the main process inside the PCSX2 environment or a registered game location.

## Contract Tests

`app/src-c/tests/smoke.sh`, `pcsx2_update_test.py`, `pcsx2_release.json`, `pcsx2_bad_archive.tar.xz`, and the C transaction suites cover initialization, updater disablement, read-only activation, repair, rollback, removal preservation, digest failure, traversal, links, architecture mismatch, CLI drift, and duplicate assets. Synthetic fixtures contain no Sony code or game content.
