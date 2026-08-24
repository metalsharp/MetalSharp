# RPCS3 environment

MetalSharp exposes RPCS3 as a supported, managed Sharp Library environment for PlayStation 3 games.

## RPCS3 ownership and paths

MetalSharp keeps the emulator runtime separate from emulator state:

```text
~/.metalsharp/emulators/rpcs3/
├── current -> versions/<release-tag>
├── previous -> versions/<release-tag>
├── versions/<release-tag>/RPCS3.app
├── home/Library/Application Support/rpcs3/
├── home/Library/Caches/rpcs3/
├── downloads/
├── staging/
├── sessions/
├── logs/
├── environment.json
└── library.json
```

RPCS3 is launched with the environment's `home` directory as `HOME`. Firmware, saves, trophies, configuration, shader caches, and installed PS3 content therefore remain isolated from a separately installed copy of RPCS3.

Removing the managed runtime only removes `versions`, `current`, `previous`, downloads, and update staging. It preserves the isolated home and every user-selected external game folder.

## Official releases and updates

MetalSharp selects an official release repository based on the host architecture. Release metadata is cached for 12 hours; the tab's **Check Updates** action bypasses that cache. Users can pin the installed build, skip the current latest build, or clear either preference without modifying emulator state.

- Apple Silicon: <https://github.com/RPCS3/rpcs3-binaries-mac-arm64/releases>
- Intel: <https://github.com/RPCS3/rpcs3-binaries-mac/releases>

An update is installed as follows:

1. Fetch official GitHub release metadata.
2. Require an architecture-matching macOS `.7z` asset, byte size, and SHA-256 digest.
3. Download into the managed downloads directory.
4. Verify the exact byte count and SHA-256 digest.
5. Extract with `unar` into an isolated staging directory.
6. Reject escaping or broken symlinks and unexpected archives without `RPCS3.app/Contents/MacOS/rpcs3`.
7. Verify the application with `codesign --verify --deep --strict`.
8. Move the app into a versioned directory on the same volume.
9. Wait for an active RPCS3 session to exit, if necessary.
10. Atomically switch `current`, preserving the old target as `previous` for rollback.

A failed download, digest, extraction, signature, move, or activation leaves the existing runtime selected. RPCS3 user state is never part of the update transaction.

`unar` is required. MetalSharp does not fall back to shell-evaluated archive commands.

## Firmware and owned content

MetalSharp does not download or bundle Sony firmware, games, keys, or licenses.

Users can select a legally acquired `PS3UPDAT.PUP`; MetalSharp invokes the managed emulator with `--headless --installfw`. User-selected PS3 packages are installed with `--headless --installpkg`. Games launch through the managed RPCS3 app with `--no-gui`, and fullscreen is enabled by default.

External game folders are references. Removing a folder from the tab only removes it from `library.json`; it never deletes the folder.

## Game discovery

The provider scans:

- the isolated RPCS3 `dev_hdd0/game` directory;
- user-selected external roots.

It reads bounded `PARAM.SFO` metadata for title, title ID, version, and category. `ICON0.PNG` is served as local card artwork. Symlinked directories are not traversed during discovery.

## Process supervision

Each launch receives its own process group and log. A session record stores its PID, provider executable, log path, and start time. Status and stop operations validate that the PID still belongs to RPCS3 before reporting or signaling it. Session records permit recovery after a MetalSharp backend restart.

## Backend API

Read endpoints:

```text
GET /emulators
GET /sharp-library/rpcs3/status
GET /sharp-library/rpcs3/games
GET /sharp-library/rpcs3/cover?id=<id>
GET /sharp-library/rpcs3/update/check
GET /sharp-library/rpcs3/update/progress
```

RPCS3 mutation endpoints:

```text
POST /sharp-library/rpcs3/scan
POST /sharp-library/rpcs3/add-root
POST /sharp-library/rpcs3/remove-root
POST /sharp-library/rpcs3/launch
POST /sharp-library/rpcs3/stop
POST /sharp-library/rpcs3/open-ui
POST /sharp-library/rpcs3/install-firmware
POST /sharp-library/rpcs3/install-package
POST /sharp-library/rpcs3/remove-runtime
POST /sharp-library/rpcs3/update/refresh
POST /sharp-library/rpcs3/update/install
POST /sharp-library/rpcs3/update/rollback
POST /sharp-library/rpcs3/pin-current
POST /sharp-library/rpcs3/unpin
POST /sharp-library/rpcs3/skip-update
POST /sharp-library/rpcs3/clear-skip
```

## Planned PlayStation 4 work

shadPS4 is not currently exposed as a MetalSharp provider. Its gated production roadmap is documented in [SHADPS4-INTEGRATION-ROADMAP.md](SHADPS4-INTEGRATION-ROADMAP.md), using the project's accurate upstream identity.
