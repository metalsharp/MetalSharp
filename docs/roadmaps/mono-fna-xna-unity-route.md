# Mono / FNA / XNA / Unity-Mono Route — Status & Roadmap
Updated: 2026-08-06

**Status: strengthened (v0.59.x).** The mono route now discovers and deploys
per-game, version-matched payloads instead of relying on a hardcoded table.

## What landed

### Discovery (`app/src-rust/src/mono_profile.rs`)
- Classifies an installed game into: **Unity-Mono** (`UnityPlayer.dll` +
  `MonoBleedingEdge/` + `*_Data/Managed`), **Unity IL2CPP** (`GameAssembly.dll`
  — routed to Wine/DXMT, never mono), **FNA**, **MonoGame**, **XNA**,
  **MonoKickstart** (`<exe>.bin.osx` / `osx/libmonosgen`), **bare .NET**.
- Unity version parsed from the `globalgamemanagers` header (verified at
  offset 48 on DREDGE 2021.3.5f1; scanned over the first 128 bytes).
- Arch from the game exe PE machine type; dependency signals (SDL2/3, Carbon,
  FAudio, FMOD, Steamworks.NET, Galaxy, BepInEx tolerated).
- **Classic root-level layouts**: XNA/MonoGame/FNA assemblies and
  Steamworks.NET/Galaxy deps are also scanned at the game ROOT (no
  `*_Data/Managed`), covering Terraria and Stardew Valley — both verified
  against real installs (Terraria: root `Microsoft.Xna.Framework.*.dll`;
  Stardew Valley: root `MonoGame.Framework.dll` + net6.0 runtimeconfig).
- Mono requirement tier per game: **Baseline** (classic XNA/FNA) vs **Modern**
  (Unity 2021+, SDL3, MonoGame) — drives the mono version story.

### Routing & bottle state
- `resolve_pipeline` consults discovery on the fallback path: mono-profile
  games → FnaArm64; IL2CPP → M11.
- `BottleManifest.mono_profile` persists the discovered profile (schema 1,
  serde-optional) for FNA bottles.

### Version-matched deployment (bottle save + repair)
- `deploy_unity_runtime`: copies the bundled `unity-mono/<lane>` runtime
  payloads into the game's `MonoBleedingEdge/EmbedRuntime` (skips the game's
  own runtime; never ships `mono-sgen`/`.version` markers), fixes install
  names, records receipts.
- `deploy_xna_assembly_set`: XNA 4.0 assembly set into the game's Managed dir.
- `deploy_profile_deps`: SDL3 + Carbon per profile.
- All deploys are idempotent, never compile, and source only from the bundle.

### Launch
- `fna_ready_check` pre-flight: mono binary + arch (explicit Rosetta error for
  x86 when `arch -x86_64` is unavailable), launch-required shims, SDL3
  payload, Unity runtime lane.
- MonoKickstart games dispatch to `launch_fna_kickstart` (bundled
  `kick.bin.osx`).
- Post-spawn health check extended from 900ms to 3×1s polling.

### Reliability
- Mono orphan sweep is prefix-scoped (foreign CrossOver/Whisky/GPTK
  processes are never killed).
- Corrupt (<1MB) cached wine-mono MSI is deleted and re-downloaded.
- Install state is per-prefix (GOG and Steam installs don't clobber).
- `steam_appid.txt` writes are reversible via `.metalsharp-original` receipts.
- No launch-time compiles: Terraria launcher / gdiplus / faudio / Xact /
  offline patcher ship prebuilt in the assets bundle
  (`assets/prebuilt-launchers/`, `assets/shims/`).

## Bundle payloads (new in the assets bundle)

| Payload | Layout | Notes |
|---|---|---|
| Unity Mono runtimes | `assets/unity-mono/{2020.3,2021.3,2022.3,6000.0}/` | arm64 `libmonosgen-2.0.1.dylib` + `libMonoPosixHelper.dylib` + `mono-sgen` + `MonoBleedingEdge.version`; `manifest.json` documents provenance |
| XNA 4.0 assembly set | `assets/xna/` | `Microsoft.Xna.Framework{,.Game,.Graphics,.Audio,.Input,.Media,.Storage}.dll` |
| SDL3 | `assets/sdl3/libSDL3.dylib` | modern FNA/MonoGame/Unity native dep |
| Prebuilt launchers | `assets/prebuilt-launchers/` | `TerrariaLauncher.exe`, `TerrariaOfflinePatcher.exe`, `Microsoft.Xna.Framework.Xact.dll` |
| gdiplus/faudio stubs | `assets/shims/libgdiplus.dylib`, `libFAudio.0.dylib` | prebuilt; no clang at launch |

Hashes are pinned in `tools/bundles/fna-unity-hashes.tsv`, enforced by
`verify-bundles.sh` (`FNA-UNITY` check) and required by the installer
(`ASSETS_REQUIRED_ARCHIVE_FILES`). Republish flow: stage payloads → run
`tools/bundles/update-fna-unity-hashes.sh <staging-root>` → rebuild
`metalsharp-assets.tar.zst` → upload to the `bundles` release tag → update
`metalsharp-bundle-manifest.tsv`.

## Known limits / follow-ups

- **Unity runtime provenance**: lanes currently carry the same arm64 Mono fork
  the FNA lane ships; Unity-specific MonoBleedingEdge forks should replace
  them per lane when published (the lane mapping keeps deployment stable).
- **No real-game launch validation yet** — the Unity-under-bundled-Mono launch
  assumption is a follow-up validation track (this PR is evidence/test-based,
  no game launches).
- **Galaxy (GOG SDK)**: DREDGE ships Galaxy assemblies; there is no Galaxy
  offline shim yet — documented limitation.
- **IL2CPP** Unity games route to Wine/DXMT (M11) and are intentionally never
  mono-routed.
- **External-library registry UI** not yet in scope; external Steam libraries
  are discovered through the existing Steam library paths.
