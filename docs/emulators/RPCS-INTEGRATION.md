# RPCS3 Managed Environment
**Updated:** 2026-09-22

MetalSharp runs RPCS3 as a managed Sharp Library environment for PlayStation 3 games. Firmware is acquired by the user from [Sony's PlayStation support page](https://www.playstation.com/en-us/support/hardware/ps3-system-software-update/); games come from the user's legally acquired content.

## Layout

```text
~/.metalsharp/emulators/rpcs3/
├── current -> versions/<release-tag>
├── previous -> versions/<release-tag>
├── versions/<release-tag>/RPCS3.app
├── home/Library/Application Support/rpcs3/
├── home/Library/Caches/rpcs3/
├── downloads/  staging/  sessions/  logs/
├── environment.json  library.json
```

RPCS3 launches with the environment's `home` as `HOME`, so firmware, saves, trophies, configuration, shader caches, and PS3 content stay isolated from any standalone RPCS3 install. Removing the managed runtime keeps the isolated home and every external game folder.

## Releases and Updates

The release repository follows host architecture, with metadata cached for 12 hours (the header's **Check RPCS3** action bypasses it):

- Apple Silicon: <https://github.com/RPCS3/rpcs3-binaries-mac-arm64/releases>
- Intel: <https://github.com/RPCS3/rpcs3-binaries-mac/releases>

Update transaction: fetch release metadata → require an architecture-matching macOS `.7z` with byte size and SHA-256 → download → verify exactly → extract with `unar` into isolated staging → reject escaping/broken symlinks and archives missing `RPCS3.app/Contents/MacOS/rpcs3` → `codesign --verify --deep --strict` → move into a versioned directory on the same volume → wait for active sessions → atomically switch `current`, keeping `previous` for rollback. Any failure leaves the existing runtime selected.

Users can pin the installed build, skip the current latest, or clear either preference.

## Firmware and Games

- **Download Firmware** opens Sony's exact support URL in the browser; **Find Games** opens `https://archive.org/`. Both links are fixed in the Electron main process.
- A user-selected `PS3UPDAT.PUP` installs through `--headless --installfw`; user-selected PS3 packages through `--headless --installpkg`.
- Games launch with `--no-gui`; fullscreen is on by default.
- External game folders are references — removing a folder changes only `library.json`.

## Game Discovery

The provider scans the isolated `dev_hdd0/game` directory and user-selected external roots, reading bounded `PARAM.SFO` metadata (title, title ID, version, category) and serving `ICON0.PNG` as local artwork. Symlinked directories are skipped.

## Process Supervision

Each launch gets its own process group and log. Session records store PID, executable, log path, and start time; status and stop operations validate the PID still belongs to RPCS3. Records survive backend restarts.

## API

```text
GET  /emulators, /sharp-library/rpcs3/{status,games,cover,update/check,update/progress}
POST /sharp-library/rpcs3/{scan,add-root,remove-root,launch,stop,open-ui,
     install-firmware,install-package,remove-runtime,update/refresh,update/install,
     update/rollback,pin-current,unpin,skip-update,clear-skip}
```

## Other Emulators

- PCSX2: [PCSX2-INTEGRATION.md](PCSX2-INTEGRATION.md)
- shadPS4: [SHADPS4-INTEGRATION.md](SHADPS4-INTEGRATION.md)
- SharpEmu: [SHARPEMU-INTEGRATION.md](SHARPEMU-INTEGRATION.md)
