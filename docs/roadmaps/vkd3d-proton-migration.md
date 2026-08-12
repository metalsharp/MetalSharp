# VKD3D -> VKD3D-Proton Graphics Stack Migration

**Date:** 2026-08-05 | **Branch:** `feat/vkd3d-vkd3d-proton` | **Base:** main @ `a77ec47c`

## 1. Source of truth (VKMT, `/Volumes/AverySSD/VKMT`)

VKMT's accepted D3D12 route (from its README, evidence-backed):

```
D3D12 application -> vkd3d-proton 3.1.0 -> Vulkan loader -> patched MoltenVK 1.4.2 -> Metal
```

- vkd3d-proton **3.1.0** (custom: NV DirectStorage meta-shader gating, TLS fix for ARM64/ARM64EC, ABI-specific PE routing)
- MoltenVK **1.4.2** (VKMT-patched; pins Vulkan portability enumeration; ICD pinned for every accepted probe)
- DXVK **3.0.2** (AArch64, ARM64EC-compatible, x86_64-routed, i386 PE builds) — d3d11/dxgi/d3d10/d3d9
- Vulkan feature contract (README + `docs/validation/moltenvk-behavior-p8-20260803/RESULTS.md`): `robustBufferAccess2`, `robustImageAccess2`, `nullDescriptor`, buffer device address, mirror-clamp-to-edge, dynamic rendering, synchronization2, maintenance4. **Not advertised:** `VK_EXT_transform_feedback`, `drawIndirectCount` (documented, evidence-backed).
- Accepted gates: device creation, command queues, resource creation, state transitions, copies, fences, deterministic readback — all four lanes (ARM64, ARM64EC/x86_64, i386/WoW64) pass per `docs/validation/d3d12-graphics-contract-p8-20260803/RESULTS.md`.

## 2. Binary inventory (full sha256)

### vkd3d-proton — D3D12 implementation (VKD3D's new core)
| File | Arch | sha256 | Size |
|---|---|---|---|
| `runtime/wine/lib/vkd3d-proton/x86_64-windows/d3d12.dll` | x86-64 PE32+ | `7a34f49a8cf309e20df8f5418c133d8e6a00882155de5532eef2bd9b9f094f93` | 446,464 bytes (loader/forwarder) |
| `runtime/wine/lib/vkd3d-proton/x86_64-windows/d3d12core.dll` | x86-64 PE32+ | `8b643bfbdc9acab92aee8c76ce971b9877f0b851cf6fe2aa04bc37cca5ac22e4` | 6,434,816 bytes (real impl) |
| `build-vkmt-i386-clang/libs/d3d12/d3d12.dll` | i386 PE32 | `52cfe58b301771dc163fd45a5c0689bf22d1bc2396133456e7f2bd94cc3b87f1` | 32-bit lane (syswow64; not wired into VKD3D — VKD3D is 64-bit only) |
| `build-vkmt-i386-clang/libs/d3d12core/d3d12core.dll` | i386 PE32 | `56abc44d741df607ccf4ae7d3cdbd801d592fba4124bccab1705661fefbeaad3` | 32-bit lane |

### MoltenVK — Vulkan-on-Metal (VKD3D's new GPU backend)
| File | Arch | sha256 |
|---|---|---|
| `runtime/wine/lib/moltenvk-vkmt/libMoltenVK.dylib` | Mach-O universal (x86_64 + arm64) | `50e41de23ce85260870c24cec11ac29b225704c6cb0366ce555dcd9ac03417f3` |
| `…/macOS/MoltenVK_icd.json` | — | `library_path: "./libMoltenVK.dylib"`, `api_version 1.4.0`, `is_portability_driver: true` |

### DXVK — d3d11/dxgi/d3d10/d3d9 surfaces (VKD3D DXGI + fallback pipelines)
| File | Arch | sha256 |
|---|---|---|
| `runtime/wine/lib/dxvk/x86_64-windows/dxgi.dll` | x86-64 PE32+ | `9c0129b1be07217fdaef8d56bb23036a9069a49444de1dddce5a9e6cea474b15` (2,727,950 bytes) |
| `…/build.64/src/d3d11/d3d11.dll` | x86-64 | `2c3c6da7ba491a60…` |
| `…/build.64/src/d3d10/d3d10core.dll` | x86-64 | `eec55f7fe60e3182…` |
| `…/build.64/src/d3d9/d3d9.dll` | x86-64 | `b9462dc3629ed6e1…` |
| `…/build.32/src/{dxgi,d3d11,d3d10core,d3d9}.dll` | i386 | `f6c71a1a…`, `111ba088…`, `02aec3f7…`, `13e6182a…` |

The table records the production artifacts used by the 2026-08-09 Elden Ring
validation. Earlier Phase 0 hashes are superseded by these patched builds.

## 3. Key architecture facts driving the implementation

1. **vkd3d-proton does NOT ship dxgi — by design.** Verified against the
   vkd3d-proton 3.1.0 source tree (`libs/` = d3d12, d3d12core, vkd3d,
   vkd3d-common, vkd3d-shader — no dxgi dir) and its README: *"vkd3d-proton
   does not supply the necessary DXGI components on its own. Instead, DXVK
   (2.1+) and vkd3d-proton share a DXGI implementation."* So VKD3D's DXGI
   comes from **DXVK's x86_64 dxgi.dll** — already wired into the VKD3D deploy
   list (`lib/dxvk/x86_64-windows/dxgi.dll`) and the bundle requirements.
   No custom dxgi needs to be built.
2. **Runtime PE layout is x86_64-windows / i386-windows** — the correct vkd3d-proton build is **build-vkmt-win64-filtered** (x86-64), NOT build-vkmt-arm64-clang (AArch64, used by VKMT's own arm64-native Wine, not MetalSharp's current x86_64-windows runtime).
3. **VKMT MoltenVK is a universal dylib** (x86_64+arm64) — fits MetalSharp's `lib/wine/x86_64-unix` and `i386-unix` needs.
4. **ICD JSON** ships with the package; `fix_moltenvk_icd_paths` already rewrites `library_path` — extend it to prefer the VKMT dylib lane.
5. **DXMT stays for DXMT/DXMT(32)**: the new lane must not touch `lib/dxmt*`.
6. **32-bit D3D12 out of scope** — VKD3D is 64-bit only. Its readiness gate
   requires only the x86_64 vkd3d-proton DLLs; DXVK i386 artifacts remain
   required independently by DXMT/DXMT(32).
7. **Vulkan feature contract is explicit** — `moltenvk_ready` should verify the runtime dylib is the VKMT one (has a recognizable feature set) and error clearly if a stock MoltenVK would be used, since vkd3d-proton requires features stock may lack.

## 4. Risks & decisions to confirm

- **dxgi ownership**: VKD3D will deploy DXVK dxgi.dll (x86_64) + vkd3d d3d12/d3d12core. d3d11/d3d10core NOT deployed for VKD3D (D3D12 games may still load d3d11 → Wine builtin or DXVK via overrides; decide in Phase 1 whether VKD3D's override string routes d3d11 to DXVK).
- **Bundle size**: +~24 MB (5.5 MB d3d12core + 18.9 MB dxvk dxgi + MoltenVK ~10 MB) on the graphics bundle. Acceptable.
- **Default `vkd3dBackend = vkd3d-proton`** (user decision): code + bundle republish must land together; migration wizard must preserve dxmt lane for rollback.
- **Hash fixtures**: test-mode hashes in installer.rs use synthetic fixtures (existing pattern) — real hashes only in the non-test const, matching how `DXMT_VKD3D_EXPECTED_HASHES` works today.

## 5. Phase 0 exit criteria
- [x] Binary inventory with full sha256 recorded
- [x] Vulkan feature contract captured from VKMT docs
- [x] dxgi ownership question resolved (DXVK provides it)
- [x] Correct vkd3d-proton build identified (win64-filtered)
- [ ] This doc committed to the branch

## 6. Next phase (Phase 1): engine.rs pipeline re-target
New VKD3D node: backend `vkd3d-proton`, deploy `d3d12.dll` + `d3d12core.dll` (+ `dxgi.dll` from DXVK), overrides `d3d12,d3d12core=n;b`, dyld paths → vkd3d-proton unix lane + VKMT MoltenVK dir, `VK_ICD_FILENAMES` → VKMT ICD. Old DXMT VKD3D node retained behind `vkd3dBackend=dxmt`.

## 7. Implemented state and final validation (2026-08-09)

The migration is implemented in the backend and deployed in the local 0.59.1
application. VKD3D resolves to `vkd3d-proton`, selects the real
`eldenring.exe`, and routes D3D12/DXGI through the isolated vkd3d-proton,
DXVK, and VKMT MoltenVK directories. The dry run reports no missing artifacts,
and the launch doctor reports `Ready for VKD3D via vkd3d-proton` with no blockers
or warnings.

Runtime refresh now preserves every graphics-owned surface:

- `dxmt`
- `dxmt_vkd3d`
- `vkd3d-proton`
- `dxvk`
- `moltenvk-vkmt`

`runtime_bundle_refresh_preserves_every_graphics_surface` covers that contract.
The production app was restarted after the migration repair; it reported
schema 4/version 0.59.1 as up to date. A canonical `bundles` release download
was then extracted and compared with the deployed VKD3D lane: the three hashes
in the inventory above are the actual bytes in both locations. The prior
recorded `9b9be309…`, `844b044e…`, and `fe9b0ec3…` values did not match any
local deployed or release payload and were corrected. The graphics archive is
republished from these verified sources and its manifest is updated atomically;
VKD3D rejects any archive or installed lane whose hashes do not match.

The backend suite passed 749 tests after adding asynchronous child reaping for
direct Wine launches. The process-level regression test requires a completed
child PID to disappear from the process table instead of remaining a zombie.

Elden Ring app `1245620` was then launched through the offline VKD3D route. The
test covered the title and offline dialogs, main menu, save loading, Chapel of
Anticipation gameplay, movement, jump, attack, mouse camera, pause/System
menus, a sustained gameplay/menu interval, saving, and Return to Desktop. The
launch log contained no device loss, resource lifetime/validation failure,
crash, or fatal assertion. Steam integrity validation separately restored the
official `Game/steam_api64.dll` (`ca1fa9a7...`), and Goldberg was re-enabled
with that official file retained in both `.orig` and the per-app cache.

After rebasing onto 0.59.1, a second smoke launch remained active and rendering
for several minutes, produced no device-loss/resource-lifetime/crash signature,
and exited without leaving a child zombie. The Goldberg re-enable path also
has a regression test proving it cannot overwrite `steam_api64.dll.orig` by
mistaking `steam_api64.orig` for the backup name.

This is an offline compatibility result, not a performance or online
anti-cheat result. MoltenVK still cannot execute D3D12 geometry shaders, some
of those PSOs fail Metal stage-interface validation, and the tested area runs
at roughly one-sixth speed with high CPU use.
