# SharpEmu Upstream Contract

This document freezes the source-backed assumptions used by MetalSharp's managed SharpEmu provider. The packaged runtime backend is C.

## Frozen research baseline

| Contract                | Frozen value                                                       |
| ----------------------- | ------------------------------------------------------------------ |
| Repository              | <https://github.com/sharpemu/sharpemu>                             |
| Researched `main`       | `600fcde63763a8109eb50e5052b3ebbeb4372dae`                         |
| Stable release          | `v0.0.3-release.3`                                                 |
| Release source commit   | `d9b599a1fdf105187156b9baad1b3737c093a46a`                         |
| macOS asset             | `sharpemu-0.0.3-release.3-osx-x64.tar.gz`                          |
| Asset ID                | `518522588`                                                        |
| Asset bytes             | `71,999,495`                                                       |
| Asset SHA-256           | `cf54f8f50c4984c0b0a6f6723ed6fbb94eb15f5f318112d3bc371156d05b681a` |
| Archive entries         | 31 regular files/directories                                       |
| Extracted bytes         | `111,974,175`                                                      |
| Main executable         | x86-64 bare Mach-O, ad-hoc signed                                  |
| Effective minimum macOS | 26.0 due to the bundled FFmpeg dylibs                              |
| Upstream notarization   | none; Gatekeeper rejects the archive executable                    |
| Runtime                 | self-contained .NET 10 (`net10.0`, SDK 10.0.103)                   |
| License                 | GPL-2.0-or-later                                                   |

Representative extracted hashes:

- `SharpEmu`: `54fec65c3e5ff9d87abc78c285a0cd33c3eb250906232a48a61c818a768c3466`
- `libMoltenVK.dylib`: `a1bbbbbf683f61094adbfe1b5da7d1da63de032702337d47b093248869b63222`
- `libvulkan.1.dylib`: `a1bbbbbf683f61094adbfe1b5da7d1da63de032702337d47b093248869b63222`
- `plugins/libavcodec.61.19.101.dylib`: `58b1d73526eb0a708387e8917d279f6383e3f7dd3de02808df6761bec6aa0dc2`

GitHub reports the release asset as mutable. MetalSharp therefore binds the release ID, asset ID, tag, exact name, URL, size, digest, and timestamps. Changed metadata for an already observed tag/asset is quarantined rather than treated as an update.

## Release-channel contract

The initial provider accepts only:

- repository `sharpemu/sharpemu`;
- the latest non-draft, non-prerelease GitHub release;
- a safe tag beginning `v` and containing a dotted version;
- no `alpha`, `beta`, or `rc` tag;
- exactly one asset named `sharpemu-<tag-without-v>-osx-x64.tar.gz`;
- an exact `https://github.com/sharpemu/sharpemu/releases/download/<tag>/<asset>` URL;
- an asset with a positive ID, bounded byte size, and 64-digit SHA-256 digest.

Rolling `osx-x64-main-*`, branch, commit, nightly, and ambiguous assets are unsupported. The website Downloads page is informational only because it was behind GitHub during research.

## Archive contract

The researched archive has a flat root containing:

```text
SharpEmu
libMoltenVK.dylib
libvulkan.1.dylib
plugins/
licenses/
LICENSE.txt
```

`plugins/` contains managed bridge assemblies and x86-64 native FFmpeg libraries. MetalSharp does not hard-code a permanent plugin count, but every release must satisfy:

- no absolute/traversing/control-character paths;
- no links, devices, sockets, FIFOs, sparse entries, duplicate paths, or case-fold collisions;
- at most 512 entries;
- at most 512 MiB extracted and 256 MiB per file;
- required executable, Vulkan loaders, plugin directory, upstream license, and license directory;
- only regular files and directories after extraction.

`lsar` performs bounded preflight and `unar` extracts into isolated same-volume staging. There is no shell or permissive archive fallback.

## macOS runtime contract

The official macOS release is x86-64. Apple Silicon runs it through Rosetta 2. MetalSharp supports x86-64 Macs directly and Apple Silicon only after a bounded Rosetta probe.

Every Mach-O is inspected recursively. Required properties are:

- an x86-64 slice;
- dependencies limited to Apple system paths or local `@rpath`, `@loader_path`, and `@executable_path` references;
- a deployment target no newer than the host;
- a valid local ad-hoc signature after MetalSharp activation.

MetalSharp records hashes before signing in `source-manifest.json`, signs native leaves and the main executable, records post-sign hashes in `activation-manifest.json`, makes the version tree read-only, and verifies hashes and signatures when activating a runtime.

The upstream archive is not notarized. The provider and UI must never represent MetalSharp's local ad-hoc signatures as upstream Developer ID signing or Apple notarization.

## CLI contract

No arguments launches the upstream Avalonia GUI. MetalSharp never uses that mode because the GUI writes beside the executable and exposes the upstream updater.

A game launch passes an `eboot.bin` plus only reviewed options:

```text
--cpu-engine=native
--log-level=info
--log-file <isolated-log>
--window-mode=windowed|exclusive
--scaling=fit
--vsync=on
<canonical-eboot.bin>
```

Supported but unavailable in MetalSharp's first provider:

- debug server;
- import tracing and strict-import diagnostics;
- manual display/resolution/refresh/HDR overrides;
- RenderDoc and dump toggles;
- native Metal backend;
- compatibility environment hacks.

The installation probe invokes a nonexistent eboot path. The inspected CLI returns exit code 2 after reporting that the file does not exist, without creating a game window. This verifies x86-64 execution, Rosetta, .NET extraction, local dependency loading, CLI parsing, and isolated write paths.

## Executable-content contract

SharpEmu accepts:

- a decrypted ELF beginning `7f 45 4c 46`; or
- a recognized fake-signed SELF beginning PS4 `4f 15 3d 1d` or PS5 `54 14 f5 ee` with the known layout identifier bytes.

SharpEmu has no retail decryption keys. MetalSharp validates only bounded leading structure and never decrypts, fake-signs, repairs, patches, or converts content.

Discovery finds a regular file named exactly `eboot.bin`. Metadata is optional and comes from `sce_sys/param.json` or adjacent `param.json`:

- `titleId`;
- `contentVersion`;
- `masterVersion`;
- `localizedParameters.defaultLanguage` and localized `titleName`.

Valid PS5 compatibility IDs match `PPSA` plus five digits. Local artwork uses `sce_sys/icon0.png`, then `pic0.png`, then `pic1.png`, after PNG signature, size, and dimension checks.

## Writable-state contract

Upstream GUI state defaults below `AppContext.BaseDirectory/user`. MetalSharp avoids the GUI and redirects reviewed mutable families:

| Upstream contract                 | MetalSharp location                   |
| --------------------------------- | ------------------------------------- |
| `HOME`                            | `home/`                               |
| .NET single-file extraction       | `cache/dotnet-bundle/<runtime-tag>/`  |
| `SHARPEMU_SAVEDATA_DIR`           | `state/saves/`                        |
| `SHARPEMU_AMPR_INDEX_CACHE`       | `cache/ampr-index/`                   |
| `SHARPEMU_VK_PIPELINE_CACHE_PATH` | `cache/vulkan/<game-id>/pipeline.bin` |
| `SHARPEMU_TEMP0_DIR`              | `writable/temp0/<session-id>/`        |
| `SHARPEMU_DOWNLOAD0_DIR`          | `writable/download0/<session-id>/`    |
| `SHARPEMU_DEVLOG_APP_DIR`         | `writable/devlog/<session-id>/`       |
| `SHARPEMU_HOSTAPP_DIR`            | `writable/hostapp/<session-id>/`      |
| `TMPDIR`                          | `writable/tmp/<session-id>/`          |
| log file                          | `logs/<game-id>-<timestamp>.log`      |

Inherited SharpEmu, debugger, profiler, dynamic-loader, proxy, RenderDoc, and diagnostic environment variables are removed before reviewed values are set. `SHARPEMU_GPU_BACKEND=metal`, `SHARPEMU_WRITABLE_APP0`, debug-server, network-redirection, and dump toggles are never set.

## Networking contract

The inspected `sceNet` implementation maps guest networking to real `System.Net.Sockets.Socket` operations, including DNS, connect, bind, listen, accept, send, and receive. Upstream has no complete network-off option.

MetalSharp policy:

- guest networking is denied by default with a `sandbox-exec` profile containing `(deny network*)`;
- the provider verifies sandbox execution and denied loopback socket creation;
- users may explicitly enable unrestricted networking in the Sharp Library and must confirm each network-enabled launch;
- session records retain whether networking was enabled;
- no network mode uploads MetalSharp diagnostics or compatibility data automatically.

This explicit opt-in is a product policy chosen for the integration; it is not an upstream containment feature.

## Process and updater contract

MetalSharp owns updates and process supervision.

- The upstream updater is never invoked.
- The no-argument GUI is never launched.
- Each game receives a process group, session record, isolated logs, runtime tag, executable identity, start time, and network-policy value.
- Launch waits until the child has executed the exact managed `SharpEmu` path.
- Recovery validates executable path and process start time before reporting or signaling a PID.
- Stop sends SIGINT, then SIGTERM, then SIGKILL after bounded waits.
- Runtime activation, rollback, and removal are blocked while a session is live.
- Failed updates leave `current` unchanged; `previous` preserves one known-good rollback target.

## Legal and privacy contract

SharpEmu is GPL-2.0-or-later and runs out of process. The official archive's `LICENSE.txt` and `licenses/` directory remain beside each runtime. MetalSharp records the exact source tag, release URL, and corresponding source repository.

MetalSharp does not download, bundle, import, decrypt, patch, upload, or provide acquisition instructions for Sony firmware, games, keys, licenses, modules, fonts, updates, DLC, or decryption material. External games are path references and are never deleted by root or runtime removal.

Compatibility pages and the FAQ open only on explicit user action. Diagnostic logs remain local and can contain title IDs, game paths, module names, and crash details.

## Maintenance procedure

Before allowlisting a new stable release:

1. Freeze the tag and source commit.
2. Record GitHub release/asset IDs, URL, size, digest, timestamps, and mutability.
3. Audit every archive path and extracted byte bound.
4. Record every Mach-O architecture, deployment target, install name, and dependency.
5. Confirm CLI parsing and nonexistent-path exit behavior.
6. Re-audit writable environment variables and all newly added diagnostic/network toggles.
7. Re-audit guest networking and sandbox behavior.
8. Run the malicious archive, transaction, mutation, discovery, path-race, supervision, state-preservation, and packaged-app suites.
9. Update this contract, `THIRD_PARTY_LICENSES`, and the capability manifest.
10. Keep the release unavailable if any contract changed without an implemented fail-closed adaptation.
