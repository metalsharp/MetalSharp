# shadPS4 Managed Environment
**Updated:** 2026-09-22

MetalSharp runs shadPS4 as an experimental managed Sharp Library provider for PlayStation 4 games. shadPS4 is early software; a game appearing in the library does not imply it is playable.

## Host Readiness

The official macOS stable core is x86_64, translated by Rosetta on Apple Silicon. Launch requires: Apple Silicon, working Rosetta 2, a host macOS version at least the executable's `LC_BUILD_VERSION` deployment target, and a passing bounded CLI capability probe.

## Layout

```text
~/.metalsharp/emulators/shadps4/
├── current -> versions/<release-tag>
├── previous -> versions/<release-tag>
├── versions/<release-tag>/     # shadps4, libvulkan.dylib, libvulkan_kosmickrisp.dylib,
│                               # kosmickrisp_mesa_icd.json, LICENSE, source.json, capabilities.json
├── home/Library/Application Support/shadPS4/
├── downloads/  staging/  sessions/  logs/
├── environment.json  library.json
```

The selected version directory is the process working directory so the upstream KosmicKrisp ICD resolves its local Vulkan driver; `HOME` points at the isolated `home`. Settings, saves, trophies, controller profiles, screenshots, patches, cheats, modules, fonts, and shader caches stay separate from a standalone shadPS4 install. Runtime removal keeps the home, logs, sessions, library manifest, and external game folders.

## Releases and Updates

The production channel uses official stable releases from <https://github.com/shadps4-emu/shadPS4/releases>. The probed baseline is `v.0.18.0`, asset `shadps4-macos-sdl-0.18.0.zip` (38,342,169 bytes, SHA-256 `3543e255…b403fed`), whose required members are `shadps4`, `libvulkan.dylib`, `libvulkan_kosmickrisp.dylib`, `kosmickrisp_mesa_icd.json` — all x86_64 Mach-O, ICD resolving `./libvulkan_kosmickrisp.dylib` relative to the working directory.

Update transaction: official metadata (12-hour cache) → one macOS SDL ZIP with positive size and GitHub SHA-256 → download to a unique `.part` file → verify size and digest → reject absolute paths, traversal, duplicates, control characters, escaping links, unsupported file types → require core, Vulkan loader, KosmicKrisp driver, and a locally-resolving ICD → require x86_64 Mach-O with a host-supported deployment target → record digest and source tag in `source.json` → ad-hoc sign the Mach-O files locally (upstream macOS assets are unsigned) and verify each signature → save the upstream GPL license → run `shadps4 --help` through Rosetta with isolated `HOME`, bounded output, and a timeout, requiring the flags MetalSharp uses → wait for sessions → atomic activation with `previous` retained. Any failure cleans staging and partial downloads and leaves `current` unchanged.

Required CLI capabilities: `-g/--game`, `-f/--fullscreen`, `--config-global`, `--add-game-folder`, `--set-addon-folder`, `--override-root`.

## Games

Games are user-dumped directories. A base game indexes when a bounded scan finds:

```text
CUSAxxxxx/eboot.bin
CUSAxxxxx/sce_sys/param.sfo
```

The bounded SFO parser reads title, CUSA ID, version, and category; `sce_sys/icon0.png` serves as local card artwork. Patch/update directories are deduplicated, symlinks are not traversed, and scans have depth, entry, metadata-size, and game-count limits. Removing a root changes only `library.json`. Games, updates, and DLC are dumped and prepared by the user.

## Optional Modules and Fonts

Decrypted firmware modules and fonts dumped from a legally owned console are optional compatibility files. Module import accepts upstream-supported `.sprx` names with ELF magic. Font import stages a complete replacement and restores the previous tree on failure. Source files are copied.

## Launch Supervision

Launches run the core directly as an argv array: runtime directory as working directory, isolated `HOME`, absolute local Vulkan ICD path, dedicated process group, per-launch log, and an atomic session record with PID, executable identity, game path, runtime tag, and start time. Status, stop, and recovery validate the PID belongs to the recorded executable; stop signals the process group and escalates after a bounded interval.

## API

```text
GET  /emulators, /sharp-library/shadps4/{status,games,cover,update/check,update/progress}
POST /sharp-library/shadps4/{scan,add-root,remove-root,import-modules,import-fonts,launch,stop,
     update/refresh,update/install,update/rollback,pin-current,unpin,skip-update,clear-skip,remove-runtime}
```

Electron path-opening IPC accepts only the isolated environment and canonical roots registered in `library.json`.

## Verification

`app/src-c/tests/shadps4_release.json`, `shadps4_bad_archive.zip`, `shadps4_update_test.py`, and `smoke.sh` cover provider registration, host rejection, SFO discovery, artwork, root preservation, module/font import, launch/stop, update handoff, rollback, state preservation, digest failure, traversal, duplicate ZIP entries, symlinks, wrong architecture, invalid ICD manifests, failed signing, failed CLI probes, and `.part` cleanup.

Reproduction commands for the probed release:

```sh
curl -fsSL https://api.github.com/repos/shadps4-emu/shadPS4/releases/latest -o /tmp/shadps4-release.json
curl -fL https://github.com/shadps4-emu/shadPS4/releases/download/v.0.18.0/shadps4-macos-sdl-0.18.0.zip -o /tmp/shadps4.zip
stat -f %z /tmp/shadps4.zip && shasum -a 256 /tmp/shadps4.zip && unzip -Z1 /tmp/shadps4.zip
/usr/bin/arch -x86_64 ./shadps4 --help
```
