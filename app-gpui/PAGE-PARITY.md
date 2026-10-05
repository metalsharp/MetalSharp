# Sharp Library and Logs preview parity

## Approved scope

All eight Sharp Library sources and the full Logs page, based on the existing Vue templates, English copy, and final CSS overrides. These remain isolated UI previews: no real account sign-in, installation, executable/emulator launch, network opt-in, filesystem scans, production log reads, or persistence.

## Implementation seams

- Shared header/footer and seven themes stay in `src/ui.rs`; search is hidden on Sharp Library/Logs, as in their Vue references.
- `src/page_palette.rs` carries theme colors into page entities without duplicating the application shell.
- `src/sharp_preview.rs` owns source selection, source-local sample states, and simulated dialogs.
- `src/logs_preview.rs` owns synthetic log records and Live/Crash Reports/Recent Files drawer state. Copy explicitly writes synthetic text to the clipboard; Clear View only clears preview memory. Library Play/Stop actions append clearly labeled preview events.

## Acceptance checks

- [ ] Installers default/empty and optional populated sample; source picker anchored below its trigger.
- [ ] GOG prefix/account/library states, independent of Epic.
- [ ] Epic support/account/library states, independent of GOG.
- [ ] GameJolt folder/library affordances.
- [ ] PCSX2 BIOS, setup, controllers, renderer, library, support and locations.
- [ ] RPCS3 firmware/package, library, support and locations.
- [ ] shadPS4 modules/fonts, library, support and locations.
- [ ] SharpEmu experimental research copy, network denied by default, layout library, support and locations.
- [ ] Logs typography, controls, drawer counts, original event-color priority, nested scrolling, auto-scroll on opening Live, copy, clear and empty crash state.
- [ ] Shared navigation/header/footer remain fixed; correct search visibility.
- [ ] Wide and narrow layouts; Light white menus/black text and correct theme icons.
- [ ] Formatting, compile, state tests, packaging, signature, screenshots and independent GPT-6 Luna review.

Unchecked items require verification; this document is not a production-functional or pixel-exact acceptance claim.

## Latest integration and user review — 2026-10-03

- Integrated Sharp and Logs into the active preview; shared navigation hides search on both pages. Embedded source icons and packaged artwork are available offline.
- Replaced the initial generic emulator toolbars with source-specific overview/sidebar layouts, collapsed support/location sections, PCSX2 setup controls, and SharpEmu's explicitly synthetic network-policy checkbox.
- Added the shared launch-settings popover, source-local card install/uninstall state, inline bottle backend selection, and logo fallback rather than misleading artwork for unrelated emulator/indie titles.
- User said the pages looked “exactly how I hoped,” requesting only more header-button spacing and taller application areas. Clarified that “length-wise” means **taller, not wider**.
- Applied 12 px header-button separation on Sharp/Logs. Sharp workspaces fill the available vertical body area, retain card widths, and scroll when content exceeds the window. Verified the expanded Installer area and visible footer at 1033 px window width in `/tmp/gpui-sharp-taller-spaced-final-window.png`; earlier wide default/source-picker images are `/tmp/gpui-sharp-wide-window.png` and `/tmp/gpui-sharp-picker-window.png`.
- Latest validation: `cargo fmt --check`, `cargo check`, all **18 tests**, local preview packaging, plist validation, and strict ad-hoc signature verification passed. Only the existing dependency future-incompatibility warning for `block v0.1.6` remains.
- Reopened the updated preview on Sharp Library at the user's prior window size. All state remains memory-only.

### Remaining verification and limitations

The user-approved appearance is not a pixel-diff certification. Exhaustive eight-source/theme/minimum-window screenshots and an independent review of the parent's final integration are still outstanding. Native installer/provider/emulator workflows are intentionally simulated. Some advanced card tools and diagnostic dialogs remain abbreviated; PCSX2 settings use sample options, and the GameJolt browser is an offline-disabled frame rather than a real provider website. No production-functional parity, distribution readiness, or Phase 1 completion is claimed.
