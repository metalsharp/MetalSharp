# M12 → VKD3D-Proton Graphics Stack Migration — Phase 0 Inventory

**Date:** 2026-08-05 | **Branch:** `feat/m12-vkd3d-proton` | **Base:** main @ `a77ec47c`

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

### vkd3d-proton — D3D12 implementation (M12's new core)
| File | Arch | sha256 | Size |
|---|---|---|---|
| `build-vkmt-win64-filtered/libs/d3d12/d3d12.dll` | x86-64 PE32+ | `15a7ad7af07120c79075dc1bc08284731c4ca53f82bb81002a14a7b5701cb535` | 143 KB (loader/forwarder) |
| `build-vkmt-win64-filtered/libs/d3d12core/d3d12core.dll` | x86-64 PE32+ | `43b92ad53843c819443b1c5d21930c36fe3e6cc7a893df6b2b18528477259631` | 5.5 MB (real impl) |
| `build-vkmt-i386-clang/libs/d3d12/d3d12.dll` | i386 PE32 | `52cfe58b301771dc163fd45a5c0689bf22d1bc2396133456e7f2bd94cc3b87f1` | 32-bit lane (syswow64; not wired into M12 — M12 is 64-bit only) |
| `build-vkmt-i386-clang/libs/d3d12core/d3d12core.dll` | i386 PE32 | `56abc44d741df607ccf4ae7d3cdbd801d592fba4124bccab1705661fefbeaad3` | 32-bit lane |

### MoltenVK — Vulkan-on-Metal (M12's new GPU backend)
| File | Arch | sha256 |
|---|---|---|
| `third_party/MoltenVK/Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib` | Mach-O universal (x86_64 + arm64) | `2f1a864cc049952b…` (full hash re-captured at bundle time) |
| `…/macOS/MoltenVK_icd.json` | — | `library_path: "./libMoltenVK.dylib"`, `api_version 1.4.0`, `is_portability_driver: true` |

### DXVK — d3d11/dxgi/d3d10/d3d9 surfaces (M12 DXGI + fallback pipelines)
| File | Arch | sha256 |
|---|---|---|
| `dxvk-vkmt-1a5919b/build.64/src/dxgi/dxgi.dll` | x86-64 PE32+ | `abbbcb6afeaa0336…` (18.9 MB) |
| `…/build.64/src/d3d11/d3d11.dll` | x86-64 | `2c3c6da7ba491a60…` |
| `…/build.64/src/d3d10/d3d10core.dll` | x86-64 | `eec55f7fe60e3182…` |
| `…/build.64/src/d3d9/d3d9.dll` | x86-64 | `b9462dc3629ed6e1…` |
| `…/build.32/src/{dxgi,d3d11,d3d10core,d3d9}.dll` | i386 | `f6c71a1a…`, `111ba088…`, `02aec3f7…`, `13e6182a…` |

> Full hashes for dxvk/MoltenVK re-captured at Phase 2 when the hash fixtures are written.

## 3. Key architecture facts driving the implementation

1. **vkd3d-proton does NOT ship dxgi — by design.** Verified against the
   vkd3d-proton 3.1.0 source tree (`libs/` = d3d12, d3d12core, vkd3d,
   vkd3d-common, vkd3d-shader — no dxgi dir) and its README: *"vkd3d-proton
   does not supply the necessary DXGI components on its own. Instead, DXVK
   (2.1+) and vkd3d-proton share a DXGI implementation."* So M12's DXGI
   comes from **DXVK's x86_64 dxgi.dll** — already wired into the M12 deploy
   list (`lib/dxvk/x86_64-windows/dxgi.dll`) and the bundle requirements.
   No custom dxgi needs to be built.
2. **Runtime PE layout is x86_64-windows / i386-windows** — the correct vkd3d-proton build is **build-vkmt-win64-filtered** (x86-64), NOT build-vkmt-arm64-clang (AArch64, used by VKMT's own arm64-native Wine, not MetalSharp's current x86_64-windows runtime).
3. **VKMT MoltenVK is a universal dylib** (x86_64+arm64) — fits MetalSharp's `lib/wine/x86_64-unix` and `i386-unix` needs.
4. **ICD JSON** ships with the package; `fix_moltenvk_icd_paths` already rewrites `library_path` — extend it to prefer the VKMT dylib lane.
5. **DXMT stays for M9/M10/M11** (and as `m12Backend=dxmt` rollback): the new lane must not touch `lib/dxmt*`.
6. **32-bit D3D12 out of scope** — M12 is 64-bit only; i386 vkd3d/dxvk material staged for future but not deployed.
7. **Vulkan feature contract is explicit** — `moltenvk_ready` should verify the runtime dylib is the VKMT one (has a recognizable feature set) and error clearly if a stock MoltenVK would be used, since vkd3d-proton requires features stock may lack.

## 4. Risks & decisions to confirm

- **dxgi ownership**: M12 will deploy DXVK dxgi.dll (x86_64) + vkd3d d3d12/d3d12core. d3d11/d3d10core NOT deployed for M12 (D3D12 games may still load d3d11 → Wine builtin or DXVK via overrides; decide in Phase 1 whether M12's override string routes d3d11 to DXVK).
- **Bundle size**: +~24 MB (5.5 MB d3d12core + 18.9 MB dxvk dxgi + MoltenVK ~10 MB) on the graphics bundle. Acceptable.
- **Default `m12Backend = vkd3d-proton`** (user decision): code + bundle republish must land together; migration wizard must preserve dxmt lane for rollback.
- **Hash fixtures**: test-mode hashes in installer.rs use synthetic fixtures (existing pattern) — real hashes only in the non-test const, matching how `DXMT_M12_EXPECTED_HASHES` works today.

## 5. Phase 0 exit criteria
- [x] Binary inventory with full sha256 recorded
- [x] Vulkan feature contract captured from VKMT docs
- [x] dxgi ownership question resolved (DXVK provides it)
- [x] Correct vkd3d-proton build identified (win64-filtered)
- [ ] This doc committed to the branch

## 6. Next phase (Phase 1): engine.rs pipeline re-target
New M12 node: backend `vkd3d-proton`, deploy `d3d12.dll` + `d3d12core.dll` (+ `dxgi.dll` from DXVK), overrides `d3d12,d3d12core=n;b`, dyld paths → vkd3d-proton unix lane + VKMT MoltenVK dir, `VK_ICD_FILENAMES` → VKMT ICD. Old DXMT M12 node retained behind `m12Backend=dxmt`.
