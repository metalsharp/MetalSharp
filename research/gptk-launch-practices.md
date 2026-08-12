# Research: GPTK / D3DMetal Windows-game launch practices on macOS

## Summary
Most command-line GPTK launches use a per-game Wine prefix plus Apple/gcenx GPTK Wine's `wine64`, either through Apple's `gameportingtoolkit <prefix> <exe>` wrapper or the equivalent `WINEPREFIX=... WINEESYNC=1 .../bin/wine64 ...` form. D3DMetal is expected to be installed next to the GPTK Wine libraries, especially under the Wine `lib/external` directory in GPTK 2/3 layouts, and Direct3D 11/12 should normally resolve to GPTK/D3DMetal's native PE DLLs rather than Wine builtins or Vulkan/DXVK. Confidence: medium-high for command shape and file placement; medium for GPTK 3.0-2-specific internals because public documentation is sparse.

## Findings

1. **Canonical launch shape is `gameportingtoolkit <prefix> <windows-exe>` or `WINEPREFIX=... wine64 <windows-exe>`.** Apple's developer page describes GPTK as an evaluation environment for running an "unmodified Windows executable on Apple silicon". Community copies of Apple's README and AppleGamingWiki show prefix creation as: `WINEPREFIX=~/my-game-prefix \`brew --prefix game-porting-toolkit\`/bin/wine64 winecfg`, then Steam/game launch as `gameportingtoolkit ~/my-game-prefix ~/Downloads/SteamSetup.exe` and `gameportingtoolkit ~/my-game-prefix 'C:\Program Files (x86)\Steam\steam.exe'`. AppleGamingWiki states the wrapper is equivalent to running Wine directly with env vars, e.g. `MTL_HUD_ENABLED=1 WINEESYNC=1 WINEPREFIX=~/my-game-prefix $(brew --prefix game-porting-toolkit)/bin/wine64 ...`. [Apple](https://developer.apple.com/games/game-porting-toolkit/) [AppleGamingWiki](https://www.applegamingwiki.com/wiki/Game_Porting_Toolkit) [README copy](https://gist.github.com/lynkos/3999f629560219a81d4e2c083a4bf5b1)

2. **gcenx GPTK 3.0-2 is a binary Wine distribution, not a different launch model.** Search results for gcenx releases list `Game Porting Toolkit 3.0-2` and nearby `3.0-1/3.0-3` tags; gcenx's repo is described as a "binary distribution of Apple's Game Porting Toolkit" / "Game porting toolkit wine sources". Therefore use the same `WINEPREFIX=... /path/to/gcenx/.../wine64` or wrapper approach, but point paths at the installed gcenx engine selected by Heroic/Whisky/manual scripts. [gcenx releases](https://github.com/Gcenx/game-porting-toolkit/releases) [gcenx repo](https://github.com/Gcenx/game-porting-toolkit)

3. **Common env vars: `WINEPREFIX`, `WINEESYNC=1`, sometimes `WINEMSYNC=1`, `MTL_HUD_ENABLED=1`, `WINEDEBUG=-all`, and recent D3DMetal feature flags.** AppleGamingWiki's direct command quote includes `WINEPREFIX`, `WINEESYNC=1`, and `MTL_HUD_ENABLED=1`. CrossOver/Whisky/GPTK community examples add `D3DM_SUPPORT_DXR=1` for DirectX Raytracing on supported hardware/OS, `ROSETTA_ADVERTISE_AVX=1` for AVX-advertising workarounds, and GPTK 3-era `D3DM_ENABLE_METALFX=1` for MetalFX/DLSS-style paths. `WINEMSYNC=1` or CrossOver's MSync setting is common in GUI frontends, while pure Apple README examples historically use esync. [AppleGamingWiki](https://www.applegamingwiki.com/wiki/Game_Porting_Toolkit) [D3DM_SUPPORT_DXR search evidence](https://applech2.com/archives/20241001-apple-game-porting-toolkit-2-beta-3.html) [Kiran blog](https://blog.lynkos.dev/posts/play-windows-games/)

4. **D3DMetal framework/library placement: copy Apple's `redist/lib/external` contents into GPTK Wine's `lib/external`, not into the prefix.** GPTK 2/3 guides repeatedly refer to `D3DMetal.framework` and `libd3dshared.dylib` under `/Applications/Game Porting Toolkit.app/Contents/Resources/wine/lib/external/` or the Homebrew Cellar's `.../game-porting-toolkit/.../lib/external/`. AppleGamingWiki gives an update recipe: `cd /Applications/Game\ Porting\ Toolkit.app/Contents/Resources/wine/lib/external` then replace `D3DMetal.framework` and `libd3dshared.dylib`; CrossOver/Whisky upgrade guides say to copy the same two objects into `Contents/Resources/Libraries/Wine/lib/external`. Older GPTK 1 instructions copied the mounted DMG `lib/` into `$(brew --prefix game-porting-toolkit)/lib/`, but current placement is best understood as Wine runtime `lib/external`. [AppleGamingWiki](https://www.applegamingwiki.com/wiki/Game_Porting_Toolkit) [myByways Whisky GPTK guide](https://mybyways.com/blog/apple-game-porting-toolkit-with-whisky) [AppleInsider install guide](https://appleinsider.com/inside/macos-sonoma/tips/how-to-install-and-use-game-porting-toolkit-in-xcode)

5. **DYLD path vars are usually unnecessary if files are in `lib/external`; use them only for custom launchers/relocated runtimes.** Public GPTK wrapper commands normally do not export `DYLD_LIBRARY_PATH` or `DYLD_FRAMEWORK_PATH`; they rely on the GPTK Wine layout. Custom launcher reports use `DYLD_FALLBACK_LIBRARY_PATH`, `CX_D3DMETALPATH`, or `CX_APPLEGPTK_LIBD3DSHARED_PATH` to point Wine/CrossOver at external `D3DMetal.framework` and `libd3dshared.dylib`, but those are workaround/front-end variables, not standard Apple README practice. [Sikarugir issue search evidence](https://github.com/Sikarugir-App/Sikarugir/issues/222) [Reddit custom launcher evidence](https://www.reddit.com/r/macgaming/comments/1uaxxj0/how_i_got_diablo_iv_running_on_an_m5_macbook_air/) [Kiran blog](https://blog.lynkos.dev/posts/play-windows-games/)

6. **Direct3D DLL policy: for D3DMetal, use native GPTK DLLs with builtin fallback (`native,builtin`) where overrides are explicit; builtin-only prevents D3DMetal.** In Apple's/gcenx GPTK Wine, the wrapper/runtime normally arranges this without manual `WINEDLLOVERRIDES`. In CrossOver/Whisky-style configurations the graphics backend (`CX_GRAPHICS_BACKEND=d3dmetal`) or bottle setting installs/uses native PE DLLs for `dxgi`, `d3d11`, `d3d12`, etc. Community override examples use `native,builtin` for D3D12 cases; examples that set `dxgi,d3d9,d3d10core,d3d11=b` are generally DXVK/builtin troubleshooting and would not be the D3DMetal path. Recommendation: do not force `d3d11/d3d12/dxgi=builtin`; if needed, prefer `WINEDLLOVERRIDES="dxgi,d3d11,d3d12=n,b"` (plus `d3d10core` for DX10/11) and keep D3D9 on DXVK or another route because D3DMetal is primarily DX11/12. [AppleGamingWiki](https://www.applegamingwiki.com/wiki/Game_Porting_Toolkit) [Sikarugir evidence](https://sikarugir.com/) [CodeWeavers D3DMetal context](https://www.codeweavers.com/blog/mjohnson/2023/6/6/wine-comes-to-macos-apple-s-game-porting-toolkit-powered-by-crossover-source-code)

7. **"No adapters found" / "DX12 not supported" usually means D3DMetal did not load or the chosen backend is wrong.** Common causes: missing/misplaced `D3DMetal.framework` or `libd3dshared.dylib`; running upstream Wine/Homebrew wine rather than GPTK/CrossOver-patched Wine; forcing builtin Direct3D DLLs or DXVK/Vulkan path for a DX12 game; using Intel Mac/unsupported macOS/hardware; or game requirements beyond D3DMetal's implemented feature set. Whisky issue #124 says a GPTK installer problem made "Wine unable to find D3DM DLLs, causing programs that require graphics to fail." Reddit/CrossOver reports show errors like "Failed to initialize dx12" and "No suitable 3D graphics adapter" when D3DMetal is unavailable or unsupported. [Whisky issue #124](https://github.com/Whisky-App/Whisky/issues/124) [CrossOver forum example](https://www.codeweavers.com/support/forums/general?t=27;msg=306788) [Reddit DX12 failure example](https://www.reddit.com/r/macgaming/comments/17d7ilp/failed_to_initialize_dx12_crossover_236_d3dmetal/)

8. **Practical exact command templates.**

```bash
# Create/configure a GPTK prefix
WINEPREFIX="$HOME/my-game-prefix" \
  "$(brew --prefix game-porting-toolkit)/bin/wine64" winecfg

# Install Steam or a game installer via Apple's wrapper
gameportingtoolkit "$HOME/my-game-prefix" "$HOME/Downloads/SteamSetup.exe"

# Launch Steam/game via wrapper
gameportingtoolkit "$HOME/my-game-prefix" 'C:\Program Files (x86)\Steam\steam.exe'

# Equivalent direct launch, useful for custom apps
MTL_HUD_ENABLED=1 \
WINEESYNC=1 \
WINEPREFIX="$HOME/my-game-prefix" \
"$(brew --prefix game-porting-toolkit)/bin/wine64" 'C:\path\to\Game.exe'

# If a custom runtime does not auto-select D3DMetal, explicit native overrides
WINEPREFIX="$HOME/my-game-prefix" \
WINEESYNC=1 \
WINEDLLOVERRIDES="dxgi,d3d11,d3d12=n,b" \
"/path/to/gcenx/Game-Porting-Toolkit-3.0-2/bin/wine64" 'C:\path\to\Game.exe'
```

Confidence: high for first four commands; medium for explicit override because GPTK normally handles it and frontends differ.

## Sources

- Kept: Apple Game Porting Toolkit (https://developer.apple.com/games/game-porting-toolkit/) — primary source for purpose: evaluate unmodified Windows executable on Apple silicon.
- Kept: AppleGamingWiki Game Porting Toolkit (https://www.applegamingwiki.com/wiki/Game_Porting_Toolkit) — best public consolidation of Apple README commands, wrapper equivalence, D3DMetal file placement, and troubleshooting.
- Kept: gcenx/game-porting-toolkit releases (https://github.com/Gcenx/game-porting-toolkit/releases) — confirms public tags including `Game Porting Toolkit 3.0-2`.
- Kept: gcenx/game-porting-toolkit repo (https://github.com/Gcenx/game-porting-toolkit) — identifies gcenx package as Apple's GPTK Wine distribution/source base.
- Kept: Copy of Apple's GPTK 2.1 README (https://gist.github.com/lynkos/3999f629560219a81d4e2c083a4bf5b1) — public mirror of Apple README-style commands.
- Kept: AppleInsider install guide (https://appleinsider.com/inside/macos-sonoma/tips/how-to-install-and-use-game-porting-toolkit-in-xcode) — corroborates early Homebrew install/copy commands.
- Kept: myByways Whisky guide (https://mybyways.com/blog/apple-game-porting-toolkit-with-whisky) — corroborates copying GPTK `lib` / external D3DMetal objects into Wine/CrossOver/Whisky runtime.
- Kept: CodeWeavers GPTK blog (https://www.codeweavers.com/blog/mjohnson/2023/6/6/wine-comes-to-macos-apple-s-game-porting-toolkit-powered-by-crossover-source-code) — authoritative context that GPTK is Wine/CrossOver-derived.
- Kept: Whisky issue #124 (https://github.com/Whisky-App/Whisky/issues/124) — direct evidence that missing D3DM DLLs cause graphics failures.
- Dropped: YouTube-only tutorials — useful for end users but hard to quote/verify and less stable than text sources.
- Dropped: SEO blog reposts and unrelated Reddit compatibility lists — redundant or game-specific without new launch mechanics.

## Gaps

- I could not directly inspect Apple's current GPTK 3.0 README or gcenx 3.0-2 release artifact contents from the available tools, so exact internal wrapper exports for that tag remain inferred from public docs and community reports.
- Public docs do not consistently document all D3DMetal private env vars; `D3DM_SUPPORT_DXR` and `D3DM_ENABLE_METALFX` are community-observed/release-note-adjacent and should be treated as optional feature flags, not required launch vars.
- Recommended next step for implementation: on a machine with GPTK 3.0-2 installed, run `otool -L` / `DYLD_PRINT_LIBRARIES=1` against a simple `dxdiag` launch and inspect `gameportingtoolkit` wrapper script/env to confirm exact loader paths.

## Supervisor coordination

No supervisor decision was needed.

## Acceptance

```acceptance-report
{
  "criteriaSatisfied": [
    {
      "id": "criterion-1",
      "status": "satisfied",
      "evidence": "Focused research brief written to research/gptk-launch-practices.md covering requested GPTK launch commands, env vars, D3DMetal placement, DLL override policy, and no-adapter/DX12 failure causes with citations and confidence notes."
    }
  ],
  "changedFiles": [
    "/Volumes/AverySSD/MetalSharp-SM6-UE-Lab/10-worktrees/metalsharp-vkd3d-fresh-proof-pr/progress.md",
    "/Volumes/AverySSD/MetalSharp-SM6-UE-Lab/10-worktrees/metalsharp-vkd3d-fresh-proof-pr/research/gptk-launch-practices.md"
  ],
  "testsAddedOrUpdated": [],
  "commandsRun": [
    {
      "command": "web_search: Apple/GPTK command and D3DMetal queries",
      "result": "passed",
      "summary": "Found Apple, AppleGamingWiki, README mirror, gcenx, Whisky, CrossOver, and troubleshooting sources."
    },
    {
      "command": "write progress.md and research/gptk-launch-practices.md",
      "result": "passed",
      "summary": "Created progress update and final research brief."
    }
  ],
  "validationOutput": [],
  "residualRisks": [
    "Could not directly fetch/inspect GPTK 3.0-2 wrapper scripts or Apple's current gated README; some GPTK 3.0-2-specific details are inferred from public search results and community documentation."
  ],
  "noStagedFiles": true,
  "notes": "No source code changed; research-only task."
}
```
