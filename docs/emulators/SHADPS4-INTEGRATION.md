# shadPS4 managed environment

MetalSharp exposes shadPS4 as an experimental managed Sharp Library provider for PlayStation 4 games. shadPS4 remains early software; a game appearing in the library does not imply that it is playable.

MetalSharp is not affiliated with Sony Interactive Entertainment or the shadPS4 project.

## Host readiness

The current official macOS stable core is an x86_64 executable designed for Rosetta translation on Apple Silicon. MetalSharp fails closed unless all of these conditions are true:

- the host is Apple Silicon;
- Rosetta 2 can execute x86_64 programs;
- the host macOS version is at least the downloaded executable's `LC_BUILD_VERSION` deployment target;
- the downloaded runtime passes its bounded CLI capability probe.

Intel Macs are not supported by the current upstream runtime. MetalSharp derives the final OS requirement from the verified executable because upstream prose can lag behind release artifacts.

## Runtime and state ownership

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
│   ├── source.json
│   └── capabilities.json
├── home/Library/Application Support/shadPS4/
├── downloads/
├── staging/
├── sessions/
├── logs/
├── environment.json
└── library.json
```

The selected version directory is the process working directory so the upstream KosmicKrisp ICD resolves its local Vulkan driver. `HOME` points to the environment's isolated `home` directory. shadPS4 settings, saves, trophies, controller profiles, screenshots, patches, cheats, modules, fonts, shader caches, and other state therefore remain separate from a standalone shadPS4 installation.

Runtime removal deletes version pointers, version directories, downloads, and staging. It preserves the isolated home, logs, sessions, library manifest, and all external game folders.

## Verified stable updates

MetalSharp uses official stable releases from <https://github.com/shadps4-emu/shadPS4/releases>. It does not use QtLauncher or nightly builds for the production channel.

The update transaction:

1. Loads official release metadata, cached for 12 hours unless manually refreshed.
2. Requires one macOS SDL ZIP with a positive byte size and GitHub-provided SHA-256.
3. Downloads to a unique `.part` file over HTTPS.
4. Verifies exact size and SHA-256 before extraction.
5. Rejects absolute paths, traversal, duplicate entries, control characters, escaping links, and unsupported extracted file types.
6. Requires the core, Vulkan loader, KosmicKrisp driver, and an ICD manifest that resolves only the local driver.
7. Requires x86_64 Mach-O files and a deployment target supported by the host.
8. Records the original verified asset digest and exact upstream source tag in `source.json`.
9. Ad-hoc signs the verified Mach-O files locally because current upstream macOS assets are unsigned, then verifies each local signature.
10. Saves the exact upstream GPL license beside the runtime.
11. Executes `shadps4 --help` through Rosetta with an isolated `HOME`, bounded output, and a timeout, requiring the launch and configuration flags MetalSharp uses.
12. Waits for active sessions, then atomically activates the new version while retaining the previous runtime for rollback.

Any failure removes staging and partial downloads and leaves the prior `current` pointer unchanged. Pin, skip, clear-skip, rollback, and runtime-removal controls never mutate emulator state.

## Games and owned content

MetalSharp accepts external directories containing already dumped games owned by the user. A base game is indexed only when a bounded scan finds both:

```text
CUSAxxxxx/eboot.bin
CUSAxxxxx/sce_sys/param.sfo
```

The bounded SFO parser reads title, CUSA ID, version, and category. `sce_sys/icon0.png` is served as local card artwork with backend size limits. Patch/update directories are not emitted as duplicate game cards. Directory symlinks are not traversed, and scans have depth, entry, metadata-size, and game-count limits.

Removing a root only removes its canonical path from `library.json`. MetalSharp never deletes external games.

MetalSharp does not extract PS4 packages. Games, updates, and DLC must be dumped and prepared by the user in layouts supported by upstream shadPS4.

## Optional modules and fonts

Some games benefit from decrypted firmware modules and fonts dumped from a legally owned console. They are optional compatibility files, not a firmware-installation requirement.

- Module import accepts only upstream-supported `.sprx` names with ELF magic, rejects links and oversized files, and atomically copies accepted files into isolated `sys_modules`.
- Font import rejects links, devices, excessive depth, excessive file counts, excessive total size, and oversized individual files. It stages a complete replacement and restores the previous font tree if activation fails.
- Source files are copied, never moved.

MetalSharp never downloads or extracts Sony update PUPs, modules, fonts, trophy keys, games, updates, DLC, licenses, keys, or decryption material. Trophy-key onboarding remains intentionally unavailable.

## Launch supervision

MetalSharp launches the official core directly as an argv array without a shell. Every launch receives:

- the selected runtime directory as its working directory;
- isolated `HOME` and an absolute local Vulkan ICD path;
- a dedicated process group;
- a per-launch log;
- an atomic session record with PID, executable identity, game path, runtime tag, log path, and start time.

Status, stop, and recovery validate that the PID still belongs to the recorded executable. Stop requests signal the process group, wait for graceful termination, and escalate only after a bounded interval. Exit status is retained separately from active session state, and the latest launch log remains available from the game card.

No Wine, GPTK, D3DMetal, Steam-bottle, or RPCS3 variables are injected into shadPS4.

## Backend API

Read endpoints:

```text
GET /emulators
GET /sharp-library/shadps4/status
GET /sharp-library/shadps4/games
GET /sharp-library/shadps4/cover?id=<id>
GET /sharp-library/shadps4/update/check
GET /sharp-library/shadps4/update/progress
```

Mutations:

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

Electron path-opening IPC independently permits only the isolated shadPS4 environment and canonical roots registered in `library.json`.

## Verification

The C smoke and update-transaction suites cover provider registration, host rejection, SFO discovery, artwork, root preservation, module/font import, process launch/stop, active-session update handoff, rollback, runtime state preservation, wrong size/digest, traversal, duplicate ZIP entries, symlinks, missing runtime files, wrong Mach-O architecture, invalid ICD manifests, failed local signing, failed CLI probes, failed activation, and `.part` cleanup.

The packaged application must additionally pass C normal/ASAN tests, Rust parity tests, frontend checks, code-sign verification, installed-backend route checks, and manual normal/narrow UI inspection.
