# Settings overlay parity

## Approved visual baseline

On 2026-10-03 the user reviewed the integrated preview and said: **“settings looks perfect now.”** Preserve this appearance; approval is not a pixel-diff certification or production-functionality claim.

The authoritative view is `app/src/renderer/components/SettingsOverlay.vue`, opened by `LibraryTopbar.vue`, not the legacy standalone Settings view. `src/settings_preview.rs` implements its eight-card overlay, shared theme palette, fixed header, scrolling body, responsive card/row layout, source-backed labels, conditional controls and safe confirmation dialogs. The header gear opens it over the existing page without changing the page selection.

Wide-window capture: [approved Settings baseline](../docs/assets/gpui-preview-settings.png). Exhaustive minimum-window/seven-theme screenshot coverage remains pending; do not infer it from visual approval or source inspection. GPUI does not expose the original backdrop blur: translucent dimming is used. Preview safety messaging and synthetic values are intentional additions.

## Safety boundary

Credentials are non-editable visual placeholders; Save selects only a fixed synthetic boolean state. Device, account, Steam/backend, installation, folder/permission, repair, cache, update, force-kill and uninstall operations change memory only. No real credential capture, persistence, network requests, executable launch, process termination, installation or user-data deletion occurs. Update confirmations explicitly describe simulation.

Run Setup Wizard opens the existing **simulated** setup flow. The independent review's claim that this enters live setup is a false positive: The normal preview binary in `src/main.rs` does not compile or start the backend/host modules, and the setup install methods in `src/ui.rs` use local flags and executor timers only. Preserve this boundary when production integration is introduced later.

## Localization

`assets/settings-locales.json` contains 77 source-backed overlay/language-picker keys for 20 locales, retaining 19 existing English fallbacks. Regenerate with `node app-gpui/extract-settings-locales.cjs`, using the existing TypeScript compiler (`TYPESCRIPT_PATH` if needed). Extraction evaluates message definitions only, before `initialLocale()`; it does not instantiate Vue, access storage or call runtime APIs. Tests check every locale's key count, title/language labels and unsupported-locale English fallback.

## Validation and review

- Parent recovered and repaired the timed-out Luna implementation, integrated the shell, corrected GPUI element/window APIs, palette alpha packing, locale use, responsive rows, credential placeholders, anchored language menu and confirmations.
- Formatting, offline compilation/tests, bundle plist and strict ad-hoc signature validation passed. Latest unit test count: **26**.
- Fresh-context GPT-6 Luna read-only review: `/tmp/gpui-settings-independent-review.settings-independent-review.md`.
- Review corrections: explicit synthetic FEX availability gate, Mac-specific localized Steam labels, and simulation-only update confirmation copy. These do not change the approved default appearance.
- Broader phase acceptance, exhaustive interaction/theme coverage, pixel comparisons and production integration remain separate work.
