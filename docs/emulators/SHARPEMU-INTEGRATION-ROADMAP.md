# SharpEmu Integration Roadmap

Status: **production phases 0–8 complete; packaged provider installed for testing on 2026-08-24**

This roadmap records the completed managed [SharpEmu](https://github.com/sharpemu/sharpemu) environment for PlayStation 5 research software in MetalSharp's Sharp Library. SharpEmu is explicitly an early-stage emulator. Its own FAQ says it cannot generally play games yet, Windows is the primary development target, and macOS support is experimental.

Every Phase 0–8 gate below has implementation and verification evidence. The provider is exposed as experimental in `/emulators`, the Sharp Library, typed Electron IPC, Rust parity contracts, the packaged C backend, and the installed MetalSharp 0.60.0 application. Phase 9 remains intentionally deferred.

## Completion evidence

- Native provider: `app/src-c/runtime/sharpemu.c` and `app/src-c/include/metalsharp_backend/sharpemu.h`.
- Transaction/security suite: `app/src-c/tests/sharpemu_update_test.py`.
- Packaged runtime backend: arm64, route `/sharp-library/sharpemu/status`, test-only probe bypass rejected.
- Official real artifact accepted: `v0.0.3-release.3`, source commit `d9b599a1fdf105187156b9baad1b3737c093a46a`, SHA-256 `cf54f8f50c4984c0b0a6f6723ed6fbb94eb15f5f318112d3bc371156d05b681a`.
- Real runtime state: `installed=true`, `runtimeValid=true`, `gpuProbeReady=true`, network denied by default, no game roots.
- Redistributable Gen5 synthetic ELF: official installed runtime returned cleanly with `ORBIS_GEN2_OK`, guest return value 0, and network denial active.
- State preservation: packaged real-runtime removal/reinstall preserved a marker and blocked removal during the update transaction.
- Installed app: `/Applications/MetalSharp.app`, version 0.60.0, eight non-empty runtime bundles, deep signature verification passed.
- Installed UI: 1200×800 and 820×760 acceptance passed with no horizontal overflow; experimental, legal-content, unsigned-runtime, and network warnings remain visible.
- Validation: normal C, ASAN/UBSAN, 691 Rust tests, Clippy, Rust formatting, TypeScript/Vite, Biome, Prettier, Clang formatting, Python compilation, documentation links, bundle verification, DMG contract, DMG mounted-app signature, and repository/package residue searches passed.

## Outcome and non-goals

The first production-capable integration would:

- download an allowlisted official macOS SharpEmu release from `sharpemu/sharpemu`;
- verify release identity, size, SHA-256, archive structure, Mach-O properties, and local activation metadata;
- run only the direct CLI game path, never the upstream desktop GUI or updater;
- isolate saves, caches, .NET extraction, temporary files, guest-writable mounts, logs, and session records under `~/.metalsharp/emulators/sharpemu/`;
- discover existing user-owned decrypted/fake-signed PS5 game layouts without copying or altering them;
- launch one externally supervised SharpEmu process per game with a minimal, explicit environment;
- make the project's experimental status and per-game compatibility uncertainty impossible to miss;
- preserve all mutable emulator state across updates, rollback, repair, and runtime removal.

The first integration will **not**:

- download, bundle, import, decrypt, patch, or provide acquisition instructions for Sony firmware, keys, licenses, games, modules, fonts, updates, DLC, or other proprietary PlayStation material;
- dump games or guide users through console exploitation, decryption, fake-signing, or key extraction;
- install PKG files or merge base games and updates;
- launch SharpEmu's no-argument desktop GUI;
- enable SharpEmu's self-updater, Discord Rich Presence, live debugger, RenderDoc support, network redirection, or diagnostic environment toggles;
- submit compatibility reports, logs, title IDs, paths, or telemetry automatically;
- claim that a discovered title is playable merely because its `eboot.bin` parses or starts;
- expose the experimental native Metal backend until upstream documents it as supported and MetalSharp verifies it independently.

## Research baseline

Research was performed on 2026-08-24 against these immutable local source snapshots:

| Item                           | Revision / value                                                                                              |
| ------------------------------ | ------------------------------------------------------------------------------------------------------------- |
| Upstream repository            | <https://github.com/sharpemu/sharpemu>                                                                        |
| Upstream `main` revision       | `600fcde63763a8109eb50e5052b3ebbeb4372dae`                                                                    |
| Inspected release tag          | `v0.0.3-release.3`                                                                                            |
| Release source revision        | `d9b599a1fdf105187156b9baad1b3737c093a46a`                                                                    |
| Official macOS asset           | `sharpemu-0.0.3-release.3-osx-x64.tar.gz`                                                                     |
| Asset size                     | `71,999,495` bytes                                                                                            |
| Asset SHA-256                  | `cf54f8f50c4984c0b0a6f6723ed6fbb94eb15f5f318112d3bc371156d05b681a`                                            |
| Extracted regular-file bytes   | `111,974,175` bytes                                                                                           |
| Archive entries                | 31; only regular files and directories                                                                        |
| Main executable                | bare `SharpEmu`, x86-64 Mach-O, ad-hoc signed                                                                 |
| Main executable minimum macOS  | 12.0                                                                                                          |
| MoltenVK payload               | `libMoltenVK.dylib` and byte-identical `libvulkan.1.dylib`, universal x86-64/arm64, ad-hoc signed             |
| FFmpeg payload                 | x86-64 dylibs under `plugins/`, unsigned                                                                      |
| Effective full-payload minimum | macOS 26.0 because every inspected FFmpeg dylib declares `minos 26.0`                                         |
| Gatekeeper                     | rejects the upstream executable; it is not Developer ID signed or notarized                                   |
| Runtime model                  | self-contained .NET 10 single-file executable (`net10.0`, SDK 10.0.103)                                       |
| Upstream license               | GPL-2.0-or-later; bundled components also include Apache-2.0, MIT, LGPL-2.1, and other dependency obligations |

The downloaded bytes were independently hashed and the archive was inspected before extraction. Representative extracted hashes were:

- `SharpEmu`: `54fec65c3e5ff9d87abc78c285a0cd33c3eb250906232a48a61c818a768c3466`
- `libMoltenVK.dylib` and `libvulkan.1.dylib`: `a1bbbbbf683f61094adbfe1b5da7d1da63de032702337d47b093248869b63222`
- `plugins/libavcodec.61.19.101.dylib`: `58b1d73526eb0a708387e8917d279f6383e3f7dd3de02808df6761bec6aa0dc2`

These values are research evidence, not a permanent update channel. Every supported release must get its own frozen fixture, capability manifest, and acceptance record.

### Important upstream facts

1. SharpEmu executes PS5 x86-64 guest instructions natively. The official macOS build is x86-64; Apple Silicon therefore requires Rosetta 2.
2. The release is a bare executable tree, not a `.app` bundle. Running the executable without arguments opens an Avalonia desktop UI; passing an `eboot.bin` starts CLI mode.
3. SharpEmu accepts a decrypted ELF or recognized fake-signed SELF. Its loader explicitly rejects a still-encrypted retail `eboot.bin` and states that it has no decryption keys.
4. The CLI contract at the inspected release supports direct game launch, log level/file, window mode, resolution, display, refresh rate, scaling, VSync, HDR, strict import resolution, import tracing, native CPU mode, and an optional debug server.
5. The upstream GUI stores `gui-settings.json`, per-game configuration, library cache, logs, saves, and pipeline cache beside the executable. Its updater replaces files in that same directory. This conflicts with versioned immutable runtimes.
6. The CLI can redirect important mutable paths through environment variables, but not every debug output has an override. MetalSharp must use a clean environment and leave all dump/debug toggles disabled.
7. The .NET single-file executable has `IncludeNativeLibrariesForSelfExtract=true`. Microsoft documents that native payloads otherwise extract under `$HOME/.net`; MetalSharp must set `DOTNET_BUNDLE_EXTRACT_BASE_DIR` to the isolated environment.
8. SharpEmu currently creates real host sockets for emulated networking. There is no inspected release switch which reliably disables all guest networking. This is a production blocker unless an upstream fail-closed switch is added or a tested macOS process sandbox denies network access.
9. GitHub marks the latest release and assets as mutable (`immutable: false`). The provider must detect changed metadata for an already observed asset and must not treat a tag alone as immutable provenance.
10. The official website's Downloads page was one release behind GitHub during research. Release discovery must use GitHub's API and must never scrape the website for update decisions.
11. The official compatibility database currently contains very few tested titles and mixed results. A successful loader start is not evidence of playability.
12. The default macOS rendering path is Vulkan through the bundled MoltenVK. Although source contains an opt-in native Metal backend, upstream comments describe portions as untested; the initial MetalSharp provider must use the default Vulkan path only.

## Security and ownership model

### Content boundary

MetalSharp may read bounded metadata and launch a user-selected game layout. It must not become a firmware, key, module, or game-content manager for PS5 software.

Allowed:

- registering an external folder the user selects;
- finding a regular `eboot.bin` beneath that folder;
- validating basic ELF/fSELF structure without decrypting or modifying it;
- reading bounded `sce_sys/param.json` metadata;
- serving bounded local `sce_sys/icon0.png`, `pic0.png`, or `pic1.png` artwork;
- passing the canonical `eboot.bin` path to the verified SharpEmu executable;
- opening SharpEmu's official FAQ and compatibility page in the system browser.

Forbidden:

- downloading or importing PS5 firmware, keys, licenses, modules, fonts, packages, game updates, or DLC;
- decrypting or fake-signing executables;
- copying adjacent proprietary modules into MetalSharp-managed storage;
- uploading hashes, metadata, logs, or game files;
- linking to piracy, exploit, key, firmware, or copyrighted-content acquisition instructions;
- deleting or changing external game folders when a root or runtime is removed.

### Threat model

Treat all of the following as untrusted:

- GitHub API responses and release descriptions;
- redirect targets, asset names, lengths, digests, and archives;
- archive paths, modes, duplicate entries, links, and special files;
- every external game root, directory entry, `param.json`, image, ELF/SELF, and adjacent module;
- persisted root, cache, policy, session, and process records;
- SharpEmu stdout/stderr and exit status;
- PIDs recovered after a backend restart;
- inherited environment variables.

The provider must fail closed on malformed data. No endpoint may concatenate a path into a shell command. All subprocesses use argument arrays, explicit environments, bounded output, process groups, and timeouts.

### Network policy decision

SharpEmu's inspected `sceNet` implementation uses `System.Net.Sockets.Socket` for host socket creation, bind, listen, accept, send, receive, DNS, and connect behavior. Launching an untrusted game dump with unrestricted networking is not acceptable by default.

The approved integration policy is:

1. MetalSharp denies all guest network operations by default with a tested `sandbox-exec` profile.
2. A default launch fails closed if the sandbox is unavailable or its loopback-denial probe fails.
3. Users may explicitly opt in to unrestricted guest networking in the Sharp Library.
4. The UI keeps the opt-in visibly dangerous and requires a second confirmation for every network-enabled launch.
5. Session records retain whether networking was enabled; no mode uploads MetalSharp telemetry or compatibility data automatically.

The sandbox test includes TCP, UDP, DNS, bind, and listen attempts from the launched process boundary. Warning text is not a substitute for enforcement in the default mode; it is the explicit consent boundary for the user-selected unrestricted mode.

## Managed filesystem contract

Runtime and mutable state must remain separate:

```text
~/.metalsharp/emulators/sharpemu/
├── current -> versions/<release-identity>
├── previous -> versions/<release-identity>
├── versions/
│   └── <release-identity>/
│       ├── SharpEmu
│       ├── libMoltenVK.dylib
│       ├── libvulkan.1.dylib
│       ├── plugins/
│       ├── licenses/
│       ├── LICENSE.txt
│       ├── source-manifest.json
│       └── activation-manifest.json
├── home/
├── state/
│   ├── saves/
│   ├── custom-configs/
│   ├── roots.json
│   ├── policy.json
│   └── environment.json
├── cache/
│   ├── dotnet-bundle/
│   ├── ampr-index/
│   └── vulkan/<title-id>/pipeline.bin
├── writable/
│   ├── tmp/<session-id>/
│   ├── temp0/<session-id>/
│   ├── download0/<session-id>/
│   ├── devlog/<session-id>/
│   └── hostapp/<session-id>/
├── downloads/
├── staging/
├── sessions/
├── logs/
└── library-cache.json
```

Requirements:

- `versions/`, `current`, and `previous` are runtime-only.
- After activation, a version directory is read-only except during an explicit repair transaction.
- Saves, caches, roots, settings, logs, .NET extraction, and writable guest mounts never live in a version directory.
- Runtime update/removal never deletes `home/`, `state/`, `cache/`, `writable/`, `sessions/`, `logs/`, or external games.
- Root removal deletes only the canonical reference in `roots.json`.
- A “clear cache” action, if later added, must enumerate exactly which recomputable cache families it removes and must never touch saves or logs.
- Runtime removal requires explicit confirmation and is rejected while a SharpEmu session or update transaction is active.

## Launch environment contract

MetalSharp must launch the direct CLI path with a minimal allowlist rather than inheriting the backend or Electron environment.

Required per-launch values include:

```text
HOME=<environment>/home
DOTNET_BUNDLE_EXTRACT_BASE_DIR=<environment>/cache/dotnet-bundle/<runtime-digest>
TMPDIR=<environment>/writable/tmp/<session-id>
SHARPEMU_SAVEDATA_DIR=<environment>/state/saves
SHARPEMU_VK_PIPELINE_CACHE_PATH=<environment>/cache/vulkan/<title-id>/pipeline.bin
SHARPEMU_AMPR_INDEX_CACHE=<environment>/cache/ampr-index
SHARPEMU_TEMP0_DIR=<environment>/writable/temp0/<session-id>
SHARPEMU_DOWNLOAD0_DIR=<environment>/writable/download0/<session-id>
SHARPEMU_DEVLOG_APP_DIR=<environment>/writable/devlog/<session-id>
SHARPEMU_HOSTAPP_DIR=<environment>/writable/hostapp/<session-id>
SHARPEMU_LOG_FILE=<environment>/logs/<session-id>-sharpemu.log
SHARPEMU_LOG_NO_COLOR=1
```

The environment builder must remove every inherited `SHARPEMU_*`, `DOTNET_*`, `DYLD_*`, `LD_*`, proxy, debugger, profiler, RenderDoc, Vulkan-layer, and diagnostic variable before adding reviewed values. In particular, it must not set:

- `SHARPEMU_WRITABLE_APP0`;
- `SHARPEMU_DEBUG_SERVER` or `--debug-server`;
- `SHARPEMU_NET_REDIRECT`;
- `SHARPEMU_RENDERDOC`;
- any shader, image, texture, memory, or video dump variable;
- `SHARPEMU_GPU_BACKEND=metal`;
- any import-loop bypass, fake-success, auto-input, or game-specific compatibility toggle.

The initial launch arguments should remain conservative:

```text
SharpEmu
--cpu-engine=native
--log-level=info
--log-file=<isolated-log>
--window-mode=windowed
--scaling=fit
--vsync=on
<canonical-eboot.bin>
```

Resolution, display, fullscreen/borderless, scaling, VSync, and HDR controls may be added only after the exact inspected runtime accepts them. Unknown flags are never passed optimistically.

## Phase 0 — Freeze upstream contracts and resolve blockers

### Tasks

- Store source-backed fixtures for:
  - latest release JSON;
  - inspected tag, release commit, asset ID, URL, size, digest, creation time, and mutability flag;
  - safe archive listing and expected top-level layout;
  - all Mach-O architectures, deployment targets, install names, and dependencies;
  - CLI usage/error output;
  - minimal valid synthetic ELF/fSELF behavior;
  - malformed/retail-encrypted image rejection;
  - `param.json` metadata and artwork layouts;
  - every mutable path and supported isolation variable.
- Write `SHARPEMU-UPSTREAM-CONTRACT.md` before implementation begins.
- Confirm release-channel policy:
  - default channel accepts only non-draft GitHub releases with tags beginning `v`;
  - default stable excludes branch/commit tags such as `osx-x64-main-*`;
  - alpha, beta, and RC tags require a separate opt-in channel and are not production defaults;
  - the asset must be exactly `sharpemu-<version>-osx-x64.tar.gz`.
- Add a no-GUI candidate probe using an intentionally nonexistent eboot path. It must terminate with the documented error, print build provenance, create no files outside the isolated probe directories, and never initialize video or networking.
- Add a redistributable synthetic ELF fixture based on upstream's native-return tests. It must exercise loader and process behavior without Sony code or assets.
- Implement and verify the approved deny-by-default, explicit-opt-in network policy described above.
- Ask upstream or independently confirm whether the macOS 26.0 FFmpeg deployment target is intentional. Until it changes, the full runtime host gate is macOS 26+.
- Decide and document local ad-hoc signing policy. The UI must state that upstream is not notarized and that MetalSharp verifies the GitHub digest before applying local signatures.
- Confirm that current MoltenVK and FFmpeg licenses permit the exact local-download/sign/run model and capture source-offer obligations.

### Exit gate

Phase 0 passes only when all contracts are represented by executable fixtures, network access is denied by construction in the default mode, explicit opt-in is recorded and confirmed, the effective macOS requirement is proven, and no test needs proprietary Sony content.

## Phase 1 — Provider skeleton and host readiness

### Tasks

- Add a native C provider module and header following the existing RPCS3, shadPS4, and PCSX2 patterns.
- Add Rust parity-only models and serialization tests; C remains the packaged runtime backend.
- Keep the provider hidden behind a development build flag.
- Report a typed status rather than a single boolean:
  - `checking_host`;
  - `unsupported_architecture`;
  - `rosetta_required`;
  - `unsupported_macos`;
  - `network_isolation_unavailable`;
  - `missing_runtime`;
  - `runtime_probe_failed`;
  - `no_game_roots`;
  - `no_games`;
  - `ready`;
  - `running`.
- Probe:
  - host architecture;
  - macOS version;
  - Rosetta availability with a bounded x86-64 process on Apple Silicon;
  - available disk space;
  - `lsar`/`unar` availability;
  - sandbox/network-denial capability;
  - Metal support and a no-window MoltenVK/Vulkan device enumeration;
  - candidate executable architecture and effective deployment target.
- Treat RAM and CPU-count observations as advisories unless upstream publishes a hard minimum.
- Never auto-install Rosetta; show the system command/instructions and require the user to choose.

### Exit gate

Status is deterministic on Intel and Apple Silicon fixtures; unsupported systems fail closed before download or launch; the provider remains absent when its build flag is off.

## Phase 2 — Secure release installation and rollback

### Release selection

- Query GitHub's releases API over HTTPS with redirect restrictions, response-size limits, timeouts, and an explicit user agent.
- Reject drafts, unexpected repositories, unexpected tag/asset grammar, multiple matching assets, missing size/digest, non-SHA-256 digests, non-HTTPS URLs, and URLs outside the exact GitHub release path.
- Bind tag, release ID, asset ID, commit, name, URL, size, digest, creation/update timestamps, and observed mutability into `source-manifest.json`.
- If an already observed asset ID/tag changes digest, size, URL, or creation metadata, quarantine the update as upstream mutation; never silently replace it.
- Seed tests with the researched `v0.0.3-release.3` facts, but do not hard-code that release forever.

### Download and extraction

- Download to a newly created regular file using shell-free process arguments.
- Enforce compressed-size and free-space limits while streaming; verify exact byte count and SHA-256 before inspection.
- Preflight with `lsar`, then extract with `unar` into a same-volume transaction directory.
- Initial safety bounds:
  - at most 512 archive entries;
  - at most 512 MiB extracted bytes;
  - at most 256 MiB per file;
  - at most 1,024 bytes per normalized relative path;
  - no absolute paths, `..`, NULs, duplicate normalized paths, case-fold collisions, links, devices, sockets, FIFOs, sparse surprises, setuid/setgid bits, ACLs, or unexpected extended attributes.
- Require exactly one runtime root and the expected executable, MoltenVK loader names, plugin directory, upstream license, and license directory.
- Reject extra executable files and malformed permission modes.

### Runtime validation

Before activation:

- require `SharpEmu` to be a regular x86-64 Mach-O executable;
- recursively inspect every Mach-O file;
- require MoltenVK to contain an x86-64 slice;
- require FFmpeg plugins to contain the expected x86-64 slice;
- compute the effective host minimum from every required component;
- reject unexpected non-system absolute dependencies and unresolved `@rpath` dependencies;
- verify local plugin install names resolve only within the candidate tree or Apple system locations;
- hash every extracted file into a pre-sign source manifest;
- run the no-GUI nonexistent-path probe under the isolated environment;
- run the synthetic ELF fixture under a timeout and network-denial harness;
- verify no candidate probe writes into its version directory.

The researched release contains unsigned FFmpeg dylibs and is not notarized. After the source archive and every pre-sign hash pass:

1. copy the candidate to a transaction-owned version directory;
2. ad-hoc sign each Mach-O leaf with explicit arguments and no timestamp;
3. sign the main executable last;
4. verify each signature with `codesign --verify --strict`;
5. record post-sign hashes and signature identities in `activation-manifest.json`;
6. make the version tree read-only;
7. re-run the no-GUI probe from the final path.

Do not describe local ad-hoc signing as upstream signing or notarization. Do not use quarantine removal as a replacement for integrity verification.

### Atomic activation

- Serialize install, repair, rollback, pin/skip, removal, and launch mutations with one provider transaction lock.
- Place versions and symlinks on the same filesystem.
- `fsync` candidate manifests and parent directories before activation.
- Activate through atomic `current` symlink replacement.
- Preserve the last known-good target as `previous`.
- On any interruption, select only a completely verified version; never infer validity from directory existence.
- Pinning and skipping are policy records, not modifications to upstream files.
- Keep at least current and previous versions; garbage collection must never delete an active/session-referenced version.

### Exit gate

Malicious-archive, wrong-digest, wrong-size, dependency, architecture, deployment-target, signature, mutation, interrupted-transaction, repair, rollback, and concurrent-action tests all pass without changing the active runtime on failure.

## Phase 3 — Isolated mutable state

### Tasks

- Create all managed directories with mode `0700` and regular files with `0600` where practical.
- Use the explicit launch environment above.
- Version .NET extraction by active runtime digest so incompatible single-file payloads do not collide.
- Keep saves shared across runtime versions; keep caches namespaced by runtime and title where format compatibility is unknown.
- Prove all writable guest mounts resolve below `writable/` and cannot traverse through symlinks.
- Before every launch, reject symlinked state roots and unexpected ownership/modes.
- Write configuration and policy through temp-file + `fsync` + atomic rename.
- Back up user-editable settings before schema migrations; restore on migration failure.
- Do not invoke SharpEmu's no-argument GUI or updater because both write beside and replace the executable.
- Strip environment variables that could re-enable dump files, write access to `/app0`, debugging, networking, or game-specific hacks.
- Test read-only version trees with actual candidate and synthetic fixture runs.

### State preservation matrix

| Operation      | Runtime versions                | Saves    | Settings/policy | Caches                     | Logs/sessions | External games                      |
| -------------- | ------------------------------- | -------- | --------------- | -------------------------- | ------------- | ----------------------------------- |
| Update         | rotate                          | preserve | preserve        | preserve/version as needed | preserve      | reference only                      |
| Rollback       | switch                          | preserve | preserve        | preserve                   | preserve      | reference only                      |
| Repair         | replace damaged runtime only    | preserve | preserve        | preserve                   | preserve      | reference only                      |
| Remove runtime | delete runtime/download/staging | preserve | preserve        | preserve                   | preserve      | reference only                      |
| Remove root    | unchanged                       | preserve | preserve        | preserve                   | preserve      | never delete; remove reference only |

### Exit gate

An instrumented launch and filesystem snapshot prove that runtime updates, launch, stop, crash, repair, rollback, and removal cannot write outside the managed mutable roots or external game-owned paths.

## Phase 4 — Bounded game discovery and local artwork

### Root policy

- Roots are chosen through a native directory picker.
- Reject empty, relative, nonexistent, non-directory, symlink, filesystem-root, home-root, MetalSharp-state, app-bundle, and runtime-version paths.
- Canonicalize once, persist canonical absolute paths, and reject duplicates and ancestor/descendant ambiguity.
- Revalidate every root before each scan and launch.
- External roots remain references only.

### Discovery policy

- Search only for a regular file named exactly `eboot.bin`.
- Do not follow directory symlinks, aliases, Finder aliases, mount-point escapes, or package links.
- Enforce global and per-root entry, depth, elapsed-time, path-length, file-size, and game-count bounds.
- Return `scannedEntries`, `truncated`, warnings, and inaccessible-root details.
- Cache by canonical path, device/inode, size, modification time, and a bounded metadata fingerprint; invalidation is deterministic.
- Stable game IDs are derived from provider + canonical path, never user-visible title alone.
- Before launch, re-open and revalidate the target with no-follow semantics and compare identity with the scan record.

### Executable validation

- Require a plausible bounded size and recognized ELF/fSELF header.
- Reject encrypted retail images with a clear message matching upstream's requirement for an already decrypted/fake-signed image.
- Do not attempt repair, decryption, fake-signing, or format conversion.
- Do not hash whole multi-gigabyte game directories during normal scan; use bounded fingerprints and validate the executable identity at launch.

### Metadata and art

- Read only bounded `sce_sys/param.json`; reject oversized, deeply nested, invalid-UTF-8, duplicate-key, non-object, and malformed data.
- Accept only reviewed fields such as localized title, `titleId`, `contentVersion`, and `masterVersion`.
- Validate title IDs with the expected `PPSA` grammar before creating compatibility links.
- Treat every string as display data; sanitize control characters and cap lengths.
- Prefer local `sce_sys/icon0.png`, then local `pic0.png`; no remote PlayStation Store artwork lookup in the first integration.
- Verify image type, byte size, pixel dimensions, and successful decode before advertising artwork.
- The cover endpoint accepts only a scanned stable ID, revalidates containment and no-follow identity, opens the file safely, emits an image-only content type, and applies byte limits.
- Do not merge base/update layouts or choose between duplicate title IDs automatically. Distinct canonical `eboot.bin` paths remain distinct cards until an explicit, tested association contract exists.

### Exit gate

Tests cover malformed JSON, decompression/image bombs, huge/sparse files, path swaps, symlink races, duplicate IDs, inaccessible/network volumes, cancellation, bounds, cache invalidation, and root removal without deleting external data.

## Phase 5 — Launch supervision, containment, and recovery

### Tasks

- Launch only `<current>/SharpEmu`; never a path from metadata, cache, or environment.
- Use argument arrays and the reviewed environment; no shell, `open`, command string, or upstream GUI.
- Set the working directory to the active version so local MoltenVK and `plugins/` resolve.
- Create one process group and one session record before reporting success.
- Store:
  - stable session ID;
  - game ID and canonical eboot identity;
  - runtime release/digest;
  - PID, process-group ID, executable identity, and process start time;
  - MetalSharp capture log and SharpEmu log paths;
  - start/end timestamps;
  - exit code or signal;
  - stop reason and bounded diagnostic summary.
- Capture stdout and stderr to a MetalSharp-owned log with bounded rotation while also passing SharpEmu an isolated `--log-file`.
- Apply the proven network-denial boundary before guest execution.
- Default to windowed, fit scaling, VSync on, debugger off, and Vulkan/MoltenVK.
- Send SIGINT to the validated process group for graceful stop, wait a bounded interval, then SIGTERM and SIGKILL if necessary.
- Never signal a recovered PID until executable path, process start time, provider runtime identity, and group ownership all match the session record.
- On backend restart, reconcile live sessions and persist terminal exit records rather than assuming “not running.”
- Block runtime activation/removal while a session is live. Update download may proceed, but activation waits for exit or remains staged.
- Delete per-session temporary guest mounts only after process-group termination; preserve logs and saves.
- Classify expected host stop separately from emulator failure, guest failure, signal termination, and MetalSharp supervision failure.

### Compatibility links and diagnostics

- “Compatibility” opens only `https://sharpemu.app/game/<validated-PPSA-id>/` in the system browser.
- If no valid title ID exists, open the general official compatibility page.
- Log export is manual and local. Before revealing or sharing logs, warn that they can contain game titles, title IDs, local paths, module names, and crash details.
- Never submit a report or upload a log automatically.

### Exit gate

Synthetic long-running, clean-exit, crash, signal, backend-restart, PID-reuse, path-swap, network-denial, concurrent-launch, update-waiting, and forced-stop tests pass with no orphaned processes or ambiguous UI state.

## Phase 6 — Backend, Electron, and Rust contracts

### Native routes

Read endpoints:

```text
GET /sharp-library/sharpemu/status
GET /sharp-library/sharpemu/games
GET /sharp-library/sharpemu/cover?id=<stable-id>
GET /sharp-library/sharpemu/update/check
GET /sharp-library/sharpemu/update/progress
GET /sharp-library/sharpemu/sessions
```

Mutation endpoints:

```text
POST /sharp-library/sharpemu/scan
POST /sharp-library/sharpemu/add-root
POST /sharp-library/sharpemu/remove-root
POST /sharp-library/sharpemu/launch
POST /sharp-library/sharpemu/stop
POST /sharp-library/sharpemu/update/refresh
POST /sharp-library/sharpemu/update/install
POST /sharp-library/sharpemu/update/rollback
POST /sharp-library/sharpemu/pin-current
POST /sharp-library/sharpemu/unpin
POST /sharp-library/sharpemu/skip-update
POST /sharp-library/sharpemu/clear-skip
POST /sharp-library/sharpemu/remove-runtime
```

There are intentionally no firmware, key, module, package, game-update, decryption, debugger, network-enable, or “open upstream UI” endpoints.

### Contract rules

- Every response has `ok`, `provider: "sharpemu"`, and either typed data or a bounded error.
- Status distinguishes host support, network isolation, installed/valid runtime, roots, games, running sessions, active version, previous version, effective minimum macOS, update transaction, and experimental capability warnings.
- Mutations reject unknown keys and wrong JSON types rather than coercing them.
- Path-bearing requests accept only native-picker results or existing provider-issued stable IDs.
- The HTTP server limits body and response sizes.
- Cover responses are binary and never embed host paths in URLs.
- C and Rust parity models use identical field names, nullability, enums, and serialization fixtures.

### Electron boundary

- Add typed preload methods only for the routes and native pickers required above.
- Native dialogs select folders; arbitrary renderer-supplied paths are rejected.
- `shell.openExternal` uses an exact allowlist for:
  - SharpEmu repository/release/FAQ URLs;
  - general compatibility URL;
  - validated per-title compatibility URLs.
- `shell.showItemInFolder`/path reveal accepts only provider-issued canonical game, state, and log paths after main-process revalidation.
- Do not expose raw filesystem, command execution, debug-server, or generic URL-opening APIs.

### Exit gate

Native route tests, malformed JSON tests, C/Rust fixture parity, TypeScript compilation, IPC allowlist tests, and preload-isolation tests pass. Packaged Electron uses the C backend and contains no dormant forbidden IPC.

## Phase 7 — Sharp Library UX

### Information architecture

Add a `SharpEmu` tab only after all prior gates pass. It must be visually and semantically distinct from RPCS3, ShadPS4, and PCSX2.

Header copy should state:

- **Experimental PlayStation 5 research environment**;
- most games do not run;
- Windows is upstream's primary target and macOS support is experimental;
- MetalSharp and SharpEmu are unaffiliated with Sony;
- MetalSharp never downloads firmware, keys, games, or proprietary modules;
- only already decrypted/fake-signed user-owned layouts can be referenced.

### Onboarding states

1. **Host blocked** — architecture, Rosetta, macOS/dependency deployment target, archive tools, GPU probe, or network containment failed.
2. **Install verified runtime** — show source repository, exact release, size, digest verification, unsigned/notarized status, local-signing explanation, and license link.
3. **Add owned game folder** — explain the expected existing layout without linking to dumping/decryption instructions.
4. **No supported layouts found** — preserve registered roots and offer rescan; do not repeat onboarding.
5. **Experimental library** — cards show local metadata, version, path, last exit, logs, and compatibility link without promising playability.

### Controls

- Install / Check update / Update / Rollback.
- Pin current / Unpin / Skip update / Clear skipped update.
- Add folder / Remove reference / Scan.
- Play / Stop.
- Open game folder / Open isolated state / Open log.
- Official FAQ / Compatibility.
- Remove runtime with a state-preservation explanation and confirmation.

Do not include “Open SharpEmu,” firmware import, module import, package install, debugger, telemetry, or compatibility-submission controls. The only networking control is the reviewed deny-by-default unrestricted-network opt-in with per-launch confirmation.

### Responsive and accessible behavior

- Match existing header tabs and emulator card density.
- Validate at 1200×800, 820×760, and narrow layouts.
- Keep action rows keyboard reachable without horizontal overflow.
- Use real buttons and accessible labels; status cannot rely on color alone.
- Long versions, paths, warnings, and errors wrap or truncate with a title/accessible description.
- Progress survives tab changes and backend refreshes.
- Empty registered roots show “No PlayStation 5 layouts found yet,” not setup again.
- A running session remains visible even if its root becomes unavailable.

### Exit gate

Manual installed-app review verifies all onboarding, empty, update, rollback, running, stopped, error, narrow, keyboard, and screen-reader-label states. No UI copy implies broad game compatibility or Sony affiliation.

## Phase 8 — Documentation, licensing, packaging, and release acceptance

### Documentation and notices

- Add `SHARPEMU-INTEGRATION.md` for the implemented behavior.
- Add `SHARPEMU-UPSTREAM-CONTRACT.md` with frozen source/release evidence.
- Update `THIRD_PARTY_LICENSES`, backend README, compatibility disclaimer, and emulator cross-links.
- Preserve upstream `LICENSE.txt`, `licenses/`, release source URL, exact tag/commit, and component notices inside each installed runtime.
- Document SharpEmu's GPL-2.0-or-later status and separate-process boundary.
- Document MoltenVK/Apache-2.0, LibAtrac9/MIT, FFmpeg-core/LGPL-2.1, Avalonia/.NET/native dependency obligations, and source locations. Run a dedicated legal/license review before release.
- Make clear that the runtime is downloaded from upstream after installation and is not bundled into the MetalSharp application or DMG.
- Do not use SharpEmu or PlayStation marks in a way that implies endorsement.

### Automated verification

Required suites:

- normal C tests with warnings as errors;
- ASAN/UBSAN C tests;
- archive/update/rollback transaction suite;
- malicious archive and metadata corpus;
- network-containment integration test;
- no-write-to-runtime/state-isolation test;
- scan bounds, cancellation, cache, and path-race tests;
- process supervision/recovery tests;
- Rust formatting, Clippy, and full tests;
- Electron TypeScript/Vite/Biome checks;
- documentation formatting, links, and freshness checks;
- DMG workflow verification;
- CodeQL and repository CI.

### Real-artifact acceptance

Using the current official macOS stable asset, without a proprietary game:

1. fetch release metadata from GitHub;
2. verify exact URL, size, digest, release/tag/commit, and mutation metadata;
3. safely extract and validate every path;
4. verify all Mach-O architectures, dependencies, install names, and deployment targets;
5. record pre-sign hashes, apply local signatures, and verify post-sign manifests;
6. run the no-GUI nonexistent-path probe;
7. run the redistributable synthetic ELF process fixture;
8. prove no network operation escapes containment;
9. prove every write stays under isolated mutable roots;
10. update, rollback, repair, remove runtime, and confirm state markers survive;
11. confirm external root markers are untouched.

### Packaged application acceptance

- Build the active C backend and Electron frontend.
- Package MetalSharp 0.60.0-or-later normally.
- Atomically install the complete app into `/Applications/MetalSharp.app`.
- Verify deep MetalSharp signatures and the installed backend architecture.
- Verify PCSX2, RPCS3, and ShadPS4 continue to work and no removed underdeveloped provider returns.
- Verify SharpEmu routes exist only when the production gate/build flag is enabled.
- Verify the installed `app.asar`, preload, and C backend expose the intended SharpEmu contracts and no forbidden IPC.
- Perform manual UI review from the installed app, not a development Electron process or temporary-home preview.
- Run install/update/rollback/removal through the packaged backend and re-check state preservation.

### Production gate

SharpEmu may become visible only when:

- every Phase 0–8 exit gate has concrete evidence;
- default network denial is enforced and the unrestricted mode requires explicit opt-in plus per-launch confirmation;
- the current official macOS artifact passes the full real-artifact suite;
- packaged-app verification passes;
- required licenses and source offers are approved;
- the UI consistently labels the environment experimental;
- no firmware, keys, modules, games, or acquisition instructions entered source, fixtures, logs, docs, app bundle, DMG, or test artifacts.

## Phase 9 — Deferred work

These are explicitly non-blocking future projects and remain unavailable until separately gated:

- upstream desktop GUI integration after a supported external data-directory contract exists and its self-updater can be disabled safely;
- native Metal backend selection after upstream macOS support and parity tests mature;
- per-game advanced environment toggles;
- live debugger integration;
- automated compatibility lookup or report drafting;
- update/base-layout association;
- save backup/export/restore UI;
- controller-profile management;
- nightly or per-commit release channels;
- remote artwork or metadata fetching;
- performance overlays and shader/texture diagnostics.

None of these features may be implied by the initial provider's status or documentation.

## Completion checklist template

Implementation is not complete until a final audit maps every item below to files, commands, output, screenshots, or installed artifacts:

- [x] Source and release revisions frozen.
- [x] Network isolation proven.
- [x] Host/Rosetta/macOS/GPU/dependency gates proven.
- [x] Secure release discovery and mutation detection proven.
- [x] Archive inspection/extraction corpus passes.
- [x] Runtime architecture/dependency/signing validation passes.
- [x] Atomic install/update/repair/rollback/recovery passes.
- [x] Runtime and mutable state are isolated.
- [x] Runtime removal preserves every mutable state family.
- [x] Root removal never deletes external content.
- [x] Bounded discovery, metadata, artwork, and path-race tests pass.
- [x] Launch supervision, stop, exit, PID recovery, and log rotation pass.
- [x] C/Rust/Electron/IPC contracts match.
- [x] Installed-app responsive/accessibility review passes.
- [x] Licensing and source-offer review passes.
- [x] Real official artifact acceptance passes.
- [x] Packaged `/Applications/MetalSharp.app` acceptance passes.
- [x] Repository and packaged searches find no forbidden Sony content or residue from removed providers.
- [x] Every CI and CodeQL job is terminal and successful.

## Primary sources

- Repository and README: <https://github.com/sharpemu/sharpemu>
- Latest GitHub release: <https://github.com/sharpemu/sharpemu/releases/latest>
- Inspected release: <https://github.com/sharpemu/sharpemu/releases/tag/v0.0.3-release.3>
- Release workflow: <https://github.com/sharpemu/sharpemu/blob/main/.github/workflows/workflow.yml>
- CLI and host behavior: <https://github.com/sharpemu/sharpemu/blob/main/src/SharpEmu.CLI/Program.cs>
- CLI project/runtime packaging: <https://github.com/sharpemu/sharpemu/blob/main/src/SharpEmu.CLI/SharpEmu.CLI.csproj>
- GUI settings: <https://github.com/sharpemu/sharpemu/blob/main/src/SharpEmu.GUI/GuiSettings.cs>
- Upstream updater: <https://github.com/sharpemu/sharpemu/blob/main/src/SharpEmu.GUI/Updater.cs>
- Save-data storage: <https://github.com/sharpemu/sharpemu/blob/main/src/SharpEmu.Libs/SaveData/SaveDataStorage.cs>
- Vulkan pipeline cache: <https://github.com/sharpemu/sharpemu/blob/main/src/SharpEmu.Libs/VideoOut/VulkanPipelineCacheStorage.cs>
- Network implementation: <https://github.com/sharpemu/sharpemu/blob/main/src/SharpEmu.Libs/Network/NetExports.cs>
- macOS MoltenVK staging: <https://github.com/sharpemu/sharpemu/blob/main/scripts/fetch-macos-moltenvk.sh>
- Bink/FFmpeg bridge: <https://github.com/sharpemu/sharpemu/blob/main/docs/bink2-bridge.md>
- SharpEmu downloads: <https://sharpemu.app/downloads/>
- SharpEmu FAQ: <https://sharpemu.app/faq/>
- SharpEmu compatibility database: <https://sharpemu.app/compatibility/>
- Microsoft .NET single-file extraction: <https://learn.microsoft.com/en-us/dotnet/core/deploying/single-file/overview>
- Microsoft macOS deployment/signing guidance: <https://learn.microsoft.com/en-us/dotnet/core/deploying/macos>
