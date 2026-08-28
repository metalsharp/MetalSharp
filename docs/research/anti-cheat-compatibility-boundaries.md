# Anti-Cheat Compatibility Boundaries

Created: 2026-05-19

Purpose: Phase 0 evidence lock for what MetalSharp can legitimately do for anti-cheat compatibility on macOS.

## Summary

MetalSharp should pursue anti-cheat compatibility through runtime correctness, transparency, signed artifacts, and vendor/developer enablement. It should not pursue bypass, spoofing, tamper evasion, hidden hooks, or fake kernel trust.

The practical distinction:

- Runtime compatibility means the game, launcher, graphics, media, input, Steam identity, and dependencies work correctly.
- Anti-cheat trust means the anti-cheat vendor and game developer accept the runtime as a supported environment.

MetalSharp can own the first. MetalSharp can prepare the second. MetalSharp cannot unilaterally grant itself trust for protected online games.

## Evidence From Valve And Epic

Valve's Steamworks Proton documentation says Proton supports some common anti-cheat middleware, including Easy Anti-Cheat and BattlEye, but the support is not automatic for every title. EAC requires support to be enabled and Linux/Unix client module assets to be shipped with the game build. BattlEye requires title-specific manual configuration with Valve or BattlEye contacts. Source: [Steam Deck and Proton](https://partner.steamgames.com/doc/steamdeck/proton?l=english).

Epic's EOS anti-cheat announcement says Wine/Proton support is included for Linux and that developers can activate anti-cheat support for Linux via Wine or Proton through the EOS Developer Portal. Source: [Epic Online Services EAC announcement](https://onlineservices.epicgames.com/news/epic-online-services-launches-anti-cheat-support-for-linux-mac-and-steam-deck).

Valve also recommends user-space anti-cheat components for best Proton results and says kernel-space solutions are not currently supported. Source: [Steam Deck and Proton](https://partner.steamgames.com/doc/steamdeck/proton?l=english).

## Wine Boundary

Wine describes itself as a compatibility layer that translates Windows API calls into POSIX calls instead of simulating internal Windows logic like a VM or emulator. Source: [WineHQ](https://www.winehq.org/).

That matters for anti-cheat:

- Wine does not provide a real Windows kernel.
- Wine can implement Windows user-mode APIs and DLL behavior.
- Wine can expose limited driver-like stubs where user-mode compatibility is enough.
- Wine cannot run arbitrary Windows kernel drivers as trusted Windows kernel code.

So a Windows `.sys` anti-cheat driver is not solved by renaming a Linux `.so` to a macOS `.dylib`. The host OS trust boundary is different.

## macOS Boundary

Apple's modern system-extension model allows some software to extend macOS without kernel-level access. DriverKit, NetworkExtension, and EndpointSecurity are Apple-approved mechanisms, but deployment requires entitlement review, signing/notarization, user approval, and in many cases explicit Apple-granted capabilities. Source: [Apple System Extensions and DriverKit](https://developer.apple.com/system-extensions/).

For MetalSharp:

- A user-space `.dylib` can provide host services to Wine or the MetalSharp runtime.
- A `.dylib` cannot become a Windows kernel driver.
- A macOS system extension cannot honestly claim to be the Windows kernel.
- EndpointSecurity can observe/block macOS events, but using it for anti-cheat compatibility would require Apple entitlement and vendor trust.
- Any deeper host component must be transparent, signed, notarized, and user-approved.

## Supported Work

Allowed and useful:

- improve Wine/runtime correctness
- ship signed and notarized runtime libraries
- preserve Steam appid/session identity accurately
- detect anti-cheat technology and report status
- detect whether a game includes Proton-compatible EAC/BattlEye assets
- classify games as vendor-supported, unsupported, unknown, or offline-only
- build crash/log bundles for vendors
- prepare an anti-cheat vendor compatibility kit
- contact game developers or anti-cheat vendors with reproducible evidence

## Forbidden Work

Do not build:

- anti-cheat bypasses
- fake Windows kernel drivers
- hidden process, module, or debugger spoofing
- kernel trust impersonation
- tamper evasion
- signature forgery
- tricks that hide MetalSharp from anti-cheat
- "make the anti-cheat think this is Windows" hacks

This is both a user-safety rule and a product strategy rule. A runtime that gets users banned is not a win.

## Current Repo Risks

The current tree still has one legacy endpoint naming issue to clean up during the anti-cheat classification phase:

- `include/metalsharp/AntiCheatDB.h` now uses evidence-backed support status strings instead of a broad compatible/incompatible boolean.

Recommended correction:

- Keep user-facing names explicit and non-evasive when the legacy offline toggle asset is retained.
- Reframe the toggle endpoint as a compatibility flag only if it is not bypass behavior.
- Keep static and launch-recipe status values aligned. Launch recipes expose structured anti-cheat status entries for detected game folders:
  - `vendor_supported`
  - `vendor_supported_on_proton_assets_present`
  - `blocked_pending_vendor_support`
  - `unsupported_kernel_driver`
  - `unknown`
  - `offline_only`
  - `user_mode_possible`
  - `native_macos_supported`

Each entry includes a reason, evidence paths, allowed actions, and forbidden actions. This keeps diagnostics explicit: Proton-compatible assets are evidence, not permission to bypass vendor policy.

## MetalSharp Classification Rule

Every detected protected game should get a reason, not just a launch failure:

```text
Anti-cheat detected: Easy Anti-Cheat
Detected assets: EasyAntiCheat_x64.dll, no easyanticheat_x64.so
Known Proton status: requires developer opt-in and Unix module depot
MetalSharp status: blocked pending vendor/developer support
Allowed next actions: offline mode if supported, collect logs, submit compatibility request
Forbidden next actions: bypass/tamper/spoof
```

## Phase 0 Conclusions

- Proton anti-cheat success is a trust/enablement story, not a bypass story.
- macOS makes this harder than Linux because Darwin and Apple's extension model differ from Linux kernel facilities.
- MetalSharp should focus on becoming a correct, signed, inspectable runtime and then make vendor support possible.
- Any anti-cheat roadmap item that sounds like hiding, spoofing, or bypassing should be rejected or renamed.
