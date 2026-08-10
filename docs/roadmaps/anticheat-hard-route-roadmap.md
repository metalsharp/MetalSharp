# WTMKT Anti-Cheat Hard-Route Roadmap
**Updated:** 2026-07-08


This roadmap covers the post-VM plan for MetalSharp anti-cheat/runtime support. The current local Linux VMs are ARM64, so they cannot serve as clean x86_64 Steam/Proton control machines. MetalSharp has to collect its own evidence and reduce the failure to concrete runtime contracts.

## Phase 1: Anti-Cheat Evidence Collector

Build a backend report that gathers the launch evidence for a Steam appid:

- Easy Anti-Cheat service logs and launcher logs.
- BattlEye launcher/service logs when present.
- Steam `gameprocess_log.txt` and `runprocess_log.txt`.
- EAC settings context: product id, sandbox id, deployment id, executable path, launcher title, downloaded module target, Wine version, exit code, and module mapping status.
- Clear next-action hints for known failures such as `Failed to map the anti-cheat module`.

The goal is not to declare success. The goal is to turn "anti-cheat failed" into a repeatable report we can diff after every Wine/runtime change.

Initial backend surface:

```http
POST /steam/anticheat-evidence
{"appid":1888160}
```

The report returns a normalized `status`, a human summary, EAC fields, Steam protected-launch fields, collected artifact paths, log tails, and next-action hints. For Rubicon, the expected current status is `module_mapping_failed`, with EAC setup exit `0`, module target `linux64`, Wine version `11.5`, launcher exit `206`, and Steam tracking `start_protected_game.exe`.

## Phase 2: Wine Module-Mapping Probe

Create a small probe that exercises the same class of module mapping that protected launchers expect from Wine:

- host module mapping,
- executable memory mapping,
- syscall dispatch expectations,
- loader transitions,
- dyld/dylib boundary failures on macOS,
- log output that can be attached to the appid evidence report.

Initial safe backend surface:

```http
POST /steam/anticheat-probe
{"appid":1888160}
```

This probe does not load or tamper with anti-cheat modules. It classifies the host/runtime boundary from inspectable evidence: host OS and architecture, Wine runtime paths, EAC module target, game-local anti-cheat module assets, binary magic (`ELF`, `PE`, or `Mach-O`), and whether the selected module implies a Linux-user-space substrate requirement on macOS. If Steam has only staged a download under `steamapps/downloading/<appid>` and the protected launcher or game executables still have unknown/null headers, the probe returns `staged_download_incomplete` instead of treating that payload as launchable. For Rubicon with completed protected-launch evidence, the expected current status is `linux_module_on_darwin_boundary`.

## Phase 3: Proton/Wine Delta Audit

Map MetalSharp Wine against Proton and upstream Wine behavior:

- `ntdll` loader and syscall dispatch,
- wineserver process and handle behavior,
- `steamclient` and `lsteamclient` behavior,
- mmap and memory protection behavior,
- pressure-vessel/container assumptions,
- anti-cheat runtime file layout and module target selection.

Initial backend surface:

```http
POST /steam/anticheat-delta-audit
{"appid":1888160}
```

This report groups the local runtime into audit surfaces:

- Wine loader/syscall baseline: `wine`, `wineserver`, Unix `ntdll.so`, and Windows `ntdll.dll` lanes.
- Wineserver state: whether a live `wineserver` process and per-user socket directory are present during runtime observation. Absence is expected in a clean idle install, but protected launch evidence should show the correct shared server boundary.
- Win32 translation contract: PE `kernel32.dll`, `user32.dll`, and `ntdll.dll` plus Unix-side `ntdll.so`, proving that Windows API calls have the Wine translation lanes required before any graphics or anti-cheat diagnosis is meaningful.
- Steam runtime bridge: Windows `steamclient.dll`/`steamclient64.dll` and whether a Proton-style `lsteamclient` bridge exists.
- Linux runtime assumptions: pressure-vessel, seccomp, and Linux namespaces, which are comparison rows on macOS rather than direct requirements.
- Darwin executable module boundary: whether the host can directly load Linux ELF modules, whether any vendor Mach-O module is present, and whether shipped Linux ELF assets imply a Linux user-space substrate.
- Graphics runtime adjacency: DXMT, DXVK, and MoltenVK assets that must stay intact while protected launch is debugged.
- Anti-cheat module contract: whether EAC selected a Linux module, whether Darwin can directly load it, and whether a vendor macOS module is present.

For Rubicon, the expected status is `blocking_delta_found`: the ordinary Wine/DXMT runtime pieces exist, but the protected launcher selected `linux64`, no vendor Mach-O module was found, and macOS cannot directly load Linux ELF modules.

## Phase 4: macOS Runtime Substrate Decision

Choose the truthful compatibility path:

- vendor-supported macOS anti-cheat module path if available,
- or a signed Linux-user-space compatibility substrate that can satisfy the protected module loader without spoofing, hiding, tamper evasion, or bypass behavior.

Initial backend surface:

```http
POST /steam/anticheat-substrate-decision
{"appid":1888160}
```

The decision report synthesizes the evidence, probe, and delta audit into one explicit result. For Rubicon, the expected current decision is `requires_linux_user_space_substrate_or_vendor_macos_asset`.

### Phase 4: Harmless Host Contract Probe

Add an endpoint that records the host contract without loading protected modules:

```http
POST /steam/anticheat-contract-probe
{"appid":1888160}
```

This endpoint uses the appid only for scoping existing logs and game-local identity. The host probe itself uses synthetic temporary data:

- anonymous read/write memory mapping followed by read/execute protection transition,
- synthetic ELF direct-load attempt through the host dynamic loader,
- Wine loader and wineserver path/state evidence,
- selected EAC module target from scoped protected-launch logs.

Expected macOS result for Elden Ring and Rubicon is `linux_elf_host_gap_confirmed`: EAC selected `linux64`, Wine reached module mapping, and the host dynamic loader does not accept Linux ELF modules directly. That does not prove anti-cheat support is impossible; it proves the next implementation target is a truthful Linux user-space substrate or vendor-supported macOS module assets, not another graphics route.

Allowed paths:

- Build a signed Linux user-space compatibility substrate for ELF module hosting.
- Obtain or document vendor-supported macOS anti-cheat module assets.
- Work with publisher/vendor enablement instead of spoofing trust.

Rejected paths:

- spoof anti-cheat host identity,
- hide MetalSharp or Wine from the protected launcher,
- fake kernel driver support,
- tamper with protected modules,
- claim online anti-cheat support before the protected module maps and launches with vendor-supported assets.

## Current Proof Target

Rubicon showed useful progress but not success: EAC EOS setup completed, protected launch downloaded the `linux64` module, Wine module mapping started under Wine 11.5, and then EAC failed with `Failed to map the anti-cheat module` / exit code 206. The custom Darwin substrate now closes that loader boundary; the proof below is the current gate.

## Current Implementation Surface

The backend now exposes all five read-only evidence surfaces described above:

- `/steam/anticheat-evidence`
- `/steam/anticheat-probe`
- `/steam/anticheat-delta-audit`
- `/steam/anticheat-substrate-decision`
- `/steam/anticheat-contract-probe`

The collector records the selected EAC system, CDN response, downloaded byte
count, Wine version, mapping result, launcher exit code, protected-launch log
context, and metadata for cached `.eac` vendor containers without inspecting
their contents. `METALSHARP_ANTICHEAT_PREFIX` can point at an absolute,
disposable prefix for reproducible launch evidence; normal calls use the
configured Steam bottle.

The installer also repairs the exact `/tmp/metalsharp-wine-deps/lib/` install
names found in older macOS Wine bundles, including the GnuTLS crypto closure,
to `@loader_path` and ad-hoc signs changed dylibs. This is required for the
EAC CDN request to reach HTTP 200, but it is not itself EAC module support.

The completion gate remains intentionally strict: a `launcherLoadClaim`, a
successful download, or a synthetic host probe is not treated as protected
module proof. The gate requires an explicit vendor-module mapping/load result
and a protected game transition. No identity spoofing, module tampering, fake
kernel support, or bypass route is part of this surface.

The pre-substrate Elden Ring baseline recorded the following concrete boundary:

- `Start_protected_game.exe` selected `linux64` under Wine 11.5.
- The repaired TLS path reached CDN HTTP 200 and downloaded 9,168,824 bytes.
- Wine then reported `Failed to map the anti-cheat module` and launcher exit
  code `206`; the current explicit substrate proof no longer reports that
  mapping failure.
- Direct `mac_x64` and `mac_arm64` deployment requests returned HTTP 403, so
  no vendor macOS module asset is available for this deployment.
- The synthetic macOS host probe accepted anonymous RW→RX memory but rejected a
  synthetic ELF through dyld, producing `linux_elf_host_gap_confirmed`.

These observations proved the diagnostic surface and isolated the remaining
module-hosting boundary. The boundary is now implemented by
`src/anticheat/linux_substrate.c`, built as the x86_64
`metalsharp_eac_substrate.dylib` CMake target, and copied into `app/native/`
for the existing Electron native-resource packaging path. The substrate is
used only by the explicit proof/launch environment; normal launches do not
start an anti-cheat process automatically.

## Real MetalSharp Wine 11.5 Module Proof

`tools/anticheat/run_eac_proof.py` is the bounded, opt-in proof command. It
accepts the external Steam-library game directory explicitly, refuses a Wine
binary outside the selected `.metalsharp/runtime/wine/bin/wine` tree, starts
only `Start_protected_game.exe`, enforces a thirty-second maximum, runs
`wineserver -k`, and kills all Windows-shaped Wine helpers that remain,
including truncated `comm` names such as `steamwebhelper.exe` and
`winedevice.exe`. It does not start Steam or `eldenring.exe`.

The CMake substrate target also generates the MetalSharp-owned ET_DYN symbol
image `app/native/metalsharp_eac_libc.so.6` from
`tools/anticheat/generate_linux_libc_elf.py`. This image is the Linux symbol
namespace consumed by the Darwin boundary; it is not vendor libc or an EAC
payload.

The command was run against the real external-drive launcher and the exact
installed MetalSharp Wine runtime. The resulting evidence (`schema`:
`metalsharp.eac-proof.v1`) records:

- Wine module mapping selected `linux64`, received CDN response `200`, and
  reported Wine `11.5` (exact runtime SHA-256
  `e621bf88dd07872b391198aee50bf1503fe18d43b7a9c0183fa23075efc61395`).
- The built x86_64 substrate was SHA-256
  `52258a6433d41bcf09028763377a4493ba27f45fe886ff3dbc9db123ccb3b1b7`, and
  its generated ELF symbol image was SHA-256
  `d8c1008d0ddf70287023c9d4b16840fb0dd4039ae055bff0a10a400f1ebb9886`.
- The real downloaded module was an ELF64 x86-64 image, 9,168,824 bytes,
  SHA-256 `4fdb641276de2a5f94c0fc4e10be28f4b4a53c47b8ecaa40461e09c322c75a8a`.
- The substrate completed the full RELA pass (`1,677` entries) and PLT pass
  (`151` entries), invoked `DT_INIT` and all six `DT_INIT_ARRAY` constructors,
  applied the ELF `PT_LOAD` protections, and resolved the launcher's real
  `a`, `b`, `c`, and `d` exports.
- The real EAC export `a` returned `1` through the Darwin TSD/ELF substrate.
- Cleanup left no Windows-shaped Wine process from the proof run, including
  Wine services, winedbg, wineserver, conhost, explorer, and detached Steam
  web helpers. The launcher is intentionally terminated at the thirty-second
  bound because the standalone probe does not provide the game transition.

This is a real protected-module load/relocation/constructor/export proof, not a
synthetic ELF probe, identity spoof, vendor-module edit, fake kernel result,
GPTK/VKMT path, alternate Wine build, or compatibility shim. It proves the
Linux EAC module surface through the exact MetalSharp Wine 11.5 → macOS
translation boundary. It does **not** yet prove a protected `eldenring.exe`
transition or an online session; those remain separate completion evidence and
must not be inferred from this module proof. An explicit Wine Steam handoff
attempt was also bounded and cleaned up; the installed Wine Steam client
reported `SteamAPI_Init() failed; connect to global user failed` before it
created a protected game process, so that attempt is recorded as an
unproven Steam-account handoff rather than being misrepresented as an EAC
failure or success.
