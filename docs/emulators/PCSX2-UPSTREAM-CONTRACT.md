# PCSX2 upstream contract

Status: **production contract**

Validated on 2026-08-24 against:

- PCSX2 source revision `3e29183a37e74cbc8c17bda8afb63c2d9bc6fd14`;
- PCSX2 documentation revision `24ba3e41793165062c1ddcb434460033471f3f8c`;
- official stable tag `v2.6.3`;
- official asset `pcsx2-v2.6.3-macos-Qt.tar.xz`.

## Release identity

MetalSharp reads <https://api.github.com/repos/PCSX2/pcsx2/releases/latest> and accepts one non-draft, non-prerelease release whose tag is exactly `v<major>.<minor>.<patch>`. It requires exactly one asset named:

```text
pcsx2-<tag>-macos-Qt.tar.xz
```

The selected release must include a positive byte size and a `sha256:` digest. The inspected `v2.6.3` asset is 28,960,388 bytes with SHA-256:

```text
cb7b9e6330f1abf0cf92c94065f7eb983d0fa8affcfe6b0ccb9c2a4ebf067f1a
```

The archive contains exactly one top-level app, `PCSX2-v2.6.3.app`, and 221 inspected entries. It contains no links. MetalSharp nevertheless rejects links, hard links, devices, FIFOs, absolute/traversing/control/non-ASCII paths, case-folded duplicate paths, a second top-level entry, more than 20,000 entries, or more than 2 GiB of declared output.

## macOS app contract

After extraction, the app must satisfy all of these checks:

- `CFBundleIdentifier`: `net.pcsx2.pcsx2`;
- `CFBundleExecutable`: `PCSX2`;
- `CFBundleShortVersionString`: selected release version;
- executable path: `Contents/MacOS/PCSX2`;
- every detected Mach-O: x86_64 and not arm64;
- deployment target: macOS 11.0 or newer, no newer than the host;
- non-system dependencies use contained `@rpath`, `@loader_path`, or `@executable_path` references;
- Developer ID team: `PTMR35SWS3`;
- hardened runtime present;
- `codesign --verify --deep --strict` succeeds;
- Gatekeeper accepts the app as a notarized Developer ID application.

The absolute install ID embedded in upstream `libshaderc_shared.1.dylib` is accepted only as that dylib's own install-name record. It is not accepted as an executable dependency path.

MetalSharp preserves the upstream signature and notarization. It does not patch or ad-hoc sign PCSX2. The installed version directory is made read-only without changing protected app-bundle modes.

## Host contract

The stable macOS app is x86_64-only.

- Intel hosts require x86_64, SSE4.1, and macOS 11 or newer.
- Apple Silicon hosts require macOS 11 or newer and a successful bounded `/usr/bin/arch -x86_64 /usr/bin/true` Rosetta probe.
- Other architectures fail closed.
- Less than 8 GiB RAM or fewer than four logical CPU threads produces an advisory, not a fabricated compatibility verdict.

The executable and actual Mach-O deployment target override prose when upstream changes.

## CLI contract

The selected stable is probed without opening its GUI:

```text
-version
-help
-testconfig
```

For `v2.6.3`, informational `-version` and `-help` print valid output and exit with status 1. `-testconfig` exits with status 0. MetalSharp accepts that exact observed informational behavior while still requiring the expected output.

Required stable options are:

```text
-batch -nogui -logfile -testconfig -setupwizard --
```

`v2.6.3` does not support `-datapath`. Upstream introduced it in commit `bd486f172970bba3c3fd1b93ffd36b426129ce5f`, first released in `v2.7.296`. MetalSharp detects it from the installed runtime's `-help` output and records the result in `capabilities.json`; it never infers support from current web documentation.

Stable game launches use fixed argv equivalent to:

```text
PCSX2 -nogui -batch -fullscreen -logfile <contained-log> -- <indexed-game>
```

`-fullscreen` is omitted when disabled. On Apple Silicon the fixed argv is prefixed with `/usr/bin/arch -x86_64`. No shell is used.

## Data and updater contract

`v2.6.3` stores mutable data below:

```text
$HOME/Library/Application Support/PCSX2/
```

MetalSharp supplies `~/.metalsharp/emulators/pcsx2/home` as `HOME` and precreates `Library/Application Support`. The runtime probe creates the expected PCSX2 directories, including BIOS, cache, covers, INI, input-profile, memory-card, savestate, texture, video, and log locations.

MetalSharp never uses `-portable`, because portable mode mixes mutable user data into the signed version store. If and only if the capability manifest records `dataPathFlag: true`, MetalSharp supplies the isolated PCSX2 data directory through `-datapath` as an additional boundary.

MetalSharp atomically forces `[AutoUpdater] CheckAtStartup = false` while PCSX2 is stopped. PCSX2 runtime changes then occur only through MetalSharp's verified, versioned, rollback-capable transaction. Before probing an update against existing state, MetalSharp creates a bounded private backup of `inis`, `gamesettings`, and `inputprofiles`. It does not rewind memory cards, saves, or savestates during rollback.

PCSX2 string lists use repeated INI keys. MetalSharp adds and removes only exact managed entries under:

```ini
[GameList]
RecursivePaths = /canonical/user/root
```

Unknown sections and unrelated values are preserved. Configuration is never changed while a managed PCSX2 process is active.

The inspected `v2.6.3` source and runtime define controller type under `[Pad1] Type` and `[Pad2] Type`. MetalSharp accepts only the upstream values `DualShock2`, `Guitar`, `Jogcon`, `NeGcon`, and `Popn`. The global renderer is `[EmuCore/GS] Renderer`; the verified macOS runtime contains Automatic (`-1`), Metal (`17`), OpenGL (`12`), Vulkan (`14`), and Software (`13`). Null rendering is intentionally not exposed. Baseline setup writes only these exact allowlisted values, `[UI] SetupWizardIncomplete = false`, and the disabled updater value in one atomic replacement. Advanced bindings and all other emulator settings remain PCSX2-owned.

## BIOS contract

Upstream `pcsx2/ps2/BiosTools.cpp` identifies normal BIOS files between 4 MiB and 8 MiB and validates the ROM directory plus `ROMVER`. MetalSharp mirrors the bounded ROMDIR/ROMVER checks for import and requires a recognized region marker and valid version/date digits.

A BIOS must be dumped by the user from a PlayStation 2 console they own. MetalSharp:

- never downloads, bundles, searches for, uploads, or diagnoses BIOS contents remotely;
- accepts only regular non-symlink files or a bounded dump directory;
- recognizes optional `.rom1`, `.rom2`, `.erom`, `.nvm`, and `.mec` companions;
- copies through private staging and atomically replaces the isolated BIOS directory;
- restores the prior valid set if replacement fails;
- returns description/region only, not BIOS contents or paths.

Official instructions: <https://pcsx2.net/docs/setup/bios/>.

## Game contract

The inspected stable source supports:

```text
.iso .bin .img .mdf .gz .cso .zso .chd
```

PCSX2 also loads homebrew `.elf` files. MetalSharp verifies ELF magic and recognizes no other executable type. `.cue`, `.toc`, `.cdr`, GS dumps, block dumps, and savestates are not normal library entries.

Discovery is bounded to 32 registered locations, depth 8, 20,000 entries, and 512 displayed games. An individually selected disc image is indexed exactly and does not opt its parent directory into scanning; a selected directory enables bounded recursive discovery. MetalSharp never follows directory or file symlinks and never hashes complete multi-gigabyte images during scanning. A bounded 32 MiB read may recover a normalized PS2 serial from an uncompressed disc image; otherwise the sanitized filename is authoritative fallback metadata. Compressed images remain filename-based unless a later versioned upstream contract supplies safe metadata.

Official disc-dumping instructions: <https://pcsx2.net/docs/setup/discs/>.

## Process contract

MetalSharp permits one managed PCSX2 process at a time, including the setup wizard and main UI. Every process has:

- isolated environment and fixed working directory;
- a dedicated process group;
- private stdout/stderr and PCSX2 log path;
- PID, start time, executable, runtime tag, content path, and start timestamp persisted atomically;
- PID-reuse checks against process start time and executable command;
- graceful group termination before forced termination;
- restart-safe stale-session cleanup and exit records.

Launch revalidates the upstream signature and identity every time. Runtime updates, BIOS import, initialization, root mutation, rollback, and removal fail while PCSX2 is active. New launches fail while an update transaction is active.

## Licensing and prohibited content

PCSX2 is GPL-3.0-or-later. Every installed runtime preserves:

- upstream `Contents/Resources/docs/GPL.html` as `LICENSE`;
- upstream `ThirdPartyLicenses.html`;
- release tag, source repository, asset URL, size, digest, signing team, and signature-preservation record.

MetalSharp does not bundle PCSX2. It downloads the official app only after user confirmation. It does not acquire Sony BIOS files, games, disc images, licenses, keys, updates, DLC, console modules, fonts, or decryption material.

## Contract tests

The active contract is exercised by:

- `app/src-c/tests/smoke.sh`;
- `app/src-c/tests/pcsx2_update_test.py`;
- `app/src-c/tests/pcsx2_release.json`;
- `app/src-c/tests/pcsx2_bad_archive.tar.xz`;
- Rust parity tests in `app/src-rust/src/emulators.rs`.

The transaction tests cover successful isolated initialization, updater disablement, read-only activation, repair, rollback/roll-forward, removal preservation, interrupted activation, size/digest failure, traversal, links, multiple top-level entries, wrong architecture, CLI drift, draft/prerelease releases, and duplicate matching assets. Synthetic fixtures contain no Sony code or game content.
