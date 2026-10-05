# Emulators
**Updated:** 2026-10-05

MetalSharp installs and runs four console emulators from the Sharp Library. Each one runs out of process in its own managed environment, separate from any standalone install of the same emulator.

| Emulator | Console | Status |
|---|---|---|
| PCSX2 | PlayStation 2 | Stable |
| RPCS3 | PlayStation 3 | Stable |
| shadPS4 | PlayStation 4 | Experimental — a listed game may not be playable |
| SharpEmu | PlayStation 5 | Experimental research — most games do not run |

BIOS, firmware, and games must come from hardware and content you own. MetalSharp ships none of them.

## Managed Environment

Every emulator uses the same layout under `~/.metalsharp/emulators/<name>/`:

```text
├── current -> versions/<tag>     # active build
├── previous -> versions/<tag>    # rollback target
├── versions/<tag>/               # read-only emulator build
├── home/                         # isolated HOME: config, saves, caches
├── downloads/  staging/  sessions/  logs/
├── environment.json  library.json
```

- **Isolation:** the emulator runs with `home/` as `HOME`, so its settings, saves, and caches never touch a standalone install.
- **Updates:** MetalSharp downloads only official stable releases. It verifies size and SHA-256, rejects unsafe archives, checks the binary, and switches `current` atomically. If any step fails, the active version stays put. Activation waits until running games exit.
- **Controls:** check for updates, install, pin the current build, skip a release, roll back to `previous`, or remove the runtime.
- **Removal:** removing the runtime deletes only the version trees and downloads. Home data, logs, and your game folders stay.
- **Game folders:** added folders are references. Removing one only edits `library.json`; nothing on disk is deleted. Symlinked directories are skipped.
- **Processes:** each launch gets its own process group, log, and session record. Stop and status check that the PID still belongs to the emulator, and records survive backend restarts.

The backend API is `GET /emulators` plus `/sharp-library/<name>/...` routes for status, games, scan, launch, stop, and update actions.

## PCSX2 (PS2)

Uses the official signed and notarized macOS app, which runs under Rosetta on Apple Silicon. The upstream signature is checked again at every launch.

1. Open **Sharp Library → PCSX2** and install the runtime.
2. Import a BIOS dumped from your own PS2 ([guide](https://pcsx2.net/docs/setup/bios/)). Only `.bin` files from 4 to 8 MiB with a recognized region are accepted.
3. In **PCSX2 Setup**, choose the controller for ports 1 and 2 and the renderer (Automatic, Metal, OpenGL, Vulkan, or Software).
4. Add a disc image or a game folder ([dumping guide](https://pcsx2.net/docs/setup/discs/)).

**Files:** ISO, BIN, IMG, MDF, GZ, CSO, ZSO, CHD, and homebrew ELF. Picking a single file indexes only that file; picking a folder scans it recursively.

**Differences:**

- PCSX2's own updater is turned off.
- A configuration backup is taken before each update.
- Rolling back warns first because savestates can depend on the version.
- Titles come from the disc serial when the image is uncompressed, and from the filename otherwise.

## RPCS3 (PS3)

1. Open **Sharp Library → RPCS3** and install the runtime. Apple Silicon and Intel Macs each get their matching official build.
2. **Download Firmware** opens [Sony's PS3 update page](https://www.playstation.com/en-us/support/hardware/ps3-system-software-update/). Install the downloaded `PS3UPDAT.PUP` from MetalSharp.
3. Install PS3 packages (`.pkg`) or add folders of game dumps.

**Differences:**

- Games are discovered in the isolated `dev_hdd0/game` folder and in any folders you add. Title and ID come from `PARAM.SFO`, and `ICON0.PNG` is used as artwork.
- Games launch fullscreen with no RPCS3 GUI.
- Release metadata is cached for 12 hours. **Check RPCS3** skips the cache.

## shadPS4 (PS4) — experimental

Requires Apple Silicon with Rosetta 2. The official macOS build is x86_64.

1. Open **Sharp Library → shadPS4** and install the runtime.
2. Add folders containing your own game dumps. A game is found when its folder contains `CUSAxxxxx/eboot.bin` and `sce_sys/param.sfo`. Patch folders are de-duplicated.
3. Optionally, import decrypted firmware modules (`.sprx`) and fonts from a console you own.

**Differences:**

- Upstream macOS builds are unsigned, so MetalSharp ad-hoc signs them locally after verifying them.
- The emulator runs from its version folder so its bundled Vulkan driver (KosmicKrisp) resolves correctly.

## SharpEmu (PS5) — experimental

Requires macOS 26+, Rosetta 2 on Apple Silicon, `lsar`/`unar`, and at least 1 GiB of free space. Vulkan runs through the bundled MoltenVK.

1. Open **Sharp Library → SharpEmu** and confirm the install.
2. Add game folders. A game is found when its folder contains `eboot.bin` (a decrypted ELF or fake-signed SELF) with a `param.json`, and its title ID has the form `PPSA#####`.

**Differences:**

- The layout adds `state/` (saves, configs, folders), `cache/` (.NET, AMPR, Vulkan), and `writable/`. All of them survive updates and removal.
- The upstream archive is unsigned and its release asset can change in place. MetalSharp pins the exact release and asset IDs, quarantines changed metadata, and ad-hoc signs the build locally.
- **Networking is off by default.** Games run under `sandbox-exec` with all network access denied, and launch fails if the sandbox can't be confirmed. Turning on **Allow unrestricted guest networking** asks for a second confirmation at every launch.
- A game whose `eboot.bin` changed since the last scan must be rescanned before it can launch.
