# shadPS4 upstream contract probe

Probe date: 2026-08-23

This document freezes the upstream facts used by MetalSharp's first production shadPS4 provider. Re-run the probe before changing release channels, asset matching, runtime layout, or CLI arguments.

## Probed release

- Repository: `shadps4-emu/shadPS4`
- Stable tag: `v.0.18.0`
- Asset: `shadps4-macos-sdl-0.18.0.zip`
- Size: `38,342,169` bytes
- GitHub API digest: `sha256:3543e255e2c9bad792ff77000f251493c9af3b32fef7ce5dab3a40906b403fed`
- Locally calculated SHA-256: `3543e255e2c9bad792ff77000f251493c9af3b32fef7ce5dab3a40906b403fed`

Required archive members:

```text
shadps4
libvulkan.dylib
libvulkan_kosmickrisp.dylib
kosmickrisp_mesa_icd.json
```

All three Mach-O files are x86_64. The core declares `LC_BUILD_VERSION minos 26.0` and SDK 26.5. The official asset is unsigned. Its ICD manifest resolves `./libvulkan_kosmickrisp.dylib` relative to the working directory.

The core executes successfully through:

```sh
/usr/bin/arch -x86_64 ./shadps4 --help
```

Required CLI capabilities observed:

```text
-g, --game
-f, --fullscreen
--config-global
--add-game-folder
--set-addon-folder
--override-root
```

`--override-root` is passed into the guest emulator filesystem and is not the host-side shadPS4 user-data root.

## Data isolation probe

Upstream path initialization uses `$HOME/Library/Application Support/shadPS4` on macOS unless a portable `user` directory exists in the process working directory.

MetalSharp therefore:

1. uses the selected runtime directory as the working directory for the relative ICD;
2. ensures that runtime versions do not contain a portable `user` directory;
3. sets `HOME` to `~/.metalsharp/emulators/shadps4/home`;
4. sets the Vulkan ICD path explicitly;
5. validates writes under the isolated home during the update-time CLI probe and launch tests.

The upstream user tree includes logs, screenshots, shader and pipeline cache, game data, temporary data, `sys_modules`, downloads, captures, cheats, patches, metadata, custom trophies, per-game configs, general cache, fonts, trophies, emulated home content, and custom modules. All are treated as persistent state.

## Reproduction commands

```sh
curl -fsSL \
  https://api.github.com/repos/shadps4-emu/shadPS4/releases/latest \
  -o /tmp/shadps4-release.json

curl -fL \
  https://github.com/shadps4-emu/shadPS4/releases/download/v.0.18.0/shadps4-macos-sdl-0.18.0.zip \
  -o /tmp/shadps4-macos-sdl-0.18.0.zip

stat -f %z /tmp/shadps4-macos-sdl-0.18.0.zip
shasum -a 256 /tmp/shadps4-macos-sdl-0.18.0.zip
unzip -Z1 /tmp/shadps4-macos-sdl-0.18.0.zip
file shadps4 libvulkan.dylib libvulkan_kosmickrisp.dylib
otool -l shadps4
codesign -dv --verbose=4 shadps4
/usr/bin/arch -x86_64 ./shadps4 --help
```

## Checked-in enforcement

- `app/src-c/tests/shadps4_release.json` freezes trusted metadata parsing.
- `app/src-c/tests/shadps4_bad_archive.zip` exercises corrupt-download cleanup.
- `app/src-c/tests/shadps4_update_test.py` generates signed architecture-specific runtime fixtures and validates the full update transaction and failure matrix.
- `app/src-c/tests/smoke.sh` validates provider routes, owned-content discovery/import, launch supervision, removal preservation, and corrupt-update failure.

A release is rejected if the API no longer provides a positive size and SHA-256, the macOS asset naming or layout changes, the required CLI flags disappear, the ICD escapes the runtime, the architecture is no longer supported by MetalSharp, or the deployment target exceeds the host.
