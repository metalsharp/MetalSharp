# MetalSharp Assorted Installer Runtime Roadmap
**Updated:** 2026-07-08


Status: Phase 9 active

This roadmap tracks the work needed to make `Install Windows Program` reliable for assorted Windows installers by treating installers as bottle-managed programs with installer-specific runtime prep, logs, repair actions, and post-install app discovery.

Core rule: the installer bootstrap route is not the game/app runtime route. Installer and launcher bootstrap work should happen in a stable bottle route first; the final detected app or game executable can then choose VKD3D, DXMT, bare Wine, or another route from its own evidence.

## Phase 1: Classify Installers Correctly

Build the installer classifier around real families:

- Generic `.msi`
- Generic 32-bit `.exe`
- Generic 64-bit `.exe`
- Java launchers, like Minecraft
- WebView launchers, like EA, Ubisoft, Battle.net, Epic, and Rockstar
- Electron/Squirrel launchers, like GOG Galaxy or itch-style apps
- Legacy .NET installers
- Console/admin tools, like `BERCon.exe`

Known launchers should install through `WineBare`, not VKD3D/DXMT. Graphics pipelines belong to the final launched game executable, not the bootstrap installer.

## Phase 2: Runtime Profiles Per Installer Family

Each installer family needs a default bottle runtime:

- Minecraft / Java launcher: Wine bottle, Java launcher profile, CEF compatibility
- EA / Ubisoft / Epic / Battle.net: WebView profile with `webview2`, `gecko`, `dotnet48`, `vcrun2019`, and `corefonts`
- GOG / Electron: Launcher profile with CEF/Electron wrapper support
- Generic `.msi`: Game install profile with MSI logging enabled
- Legacy .NET: Win32/Win64 .NET profile with real `dotnet48`
- BattlEye/EAC tools: only classify as anti-cheat runtime if the artifact actually contains service/runtime assets

## Phase 3: Deep Installer Logs

Every installer launch should automatically create and surface:

- top-level bottle launch log
- Wine stderr/stdout log
- MSI log if an MSI is invoked
- bootstrapper log discovery, like EA's `EA_app_*.log`
- detected failure code summary
- extracted custom-action/runtime hints when available

For EA specifically, the system should surface:

```text
EA App failed after MSI apply:
0x80070643 -> INST-14-1603
Likely failing inside MSI/custom-action stage.
```

## Phase 4: Fresh Bottle Retest Matrix

Use clean bottles for each proof target:

1. Minecraft MSI
2. EA App installer
3. Ubisoft Connect installer
4. Battle.net installer
5. Epic Games Launcher installer
6. GOG Galaxy installer
7. One generic small game/demo `.exe`
8. One generic `.msi`
9. Real BattlEye Steam title with shipped `BEService` assets

Each test records:

- install result
- detected app result
- launch result
- login window result
- child game process result
- graphics route
- missing runtime/component
- failure evidence path

## Phase 5: Post-Install App Detection

After an installer exits, scan the bottle for real installed apps while filtering junk.

Ignore:

- uninstallers
- updater helpers
- Windows Media Player
- Windows NT/system tools
- crash reporters
- redistributable installers
- helper services unless explicitly relevant

Prefer:

- launcher executable
- game executable
- signed/product-named executable
- Start Menu shortcuts
- uninstall registry display names

## Phase 6: Launcher CEF/WebView Survival

For launchers that install but render blank:

- detect CEF assets
- wrap launcher exe
- preserve real exe as `<name>_real.exe`
- inject CEF-safe flags
- hook child process creation
- add WebView2/Gecko repair buttons
- log renderer/GPU subprocess command lines

Minecraft is the hard proof case here. EA, Ubisoft, and Epic likely reuse the same class of fix.

## Phase 7: Steam-Adjacent Launcher Flow

For Steam games requiring EA, Ubisoft, or Battle.net:

- Steam remains the session/identity owner
- the game bottle becomes launch-authoritative for runtime assets
- third-party launchers install into the game's bottle or linked launcher bottle
- Steam launch should not fail because assets are outside the active bottle
- child launcher/game process should inherit the route-specific environment correctly

## Phase 8: Repair Actions Per Failure

Add targeted repairs instead of blind reruns:

- `dotnet48`
- `webview2`
- `gecko`
- `vcrun2019`
- `directx_jun2010`
- `corefonts`
- `winver`
- MSI logging retry
- CEF wrapper reapply
- launcher cache reset
- bottle component doctor

For EA, the immediate next repair path is: fresh WebView bottle, bare Wine bootstrap, `dotnet48` preinstalled, then inspect the direct MSI log.

## Phase 9: Anti-Cheat Proof Path

Do not use random downloads as proof unless they are actual runtime/service assets.

Implementation checkpoint:

- Steam game bottles now scan ordinary game folders, not just `_CommonRedist`, for Easy Anti-Cheat and BattlEye setup/service assets.
- Runtime assets infer bottle components for `easyanticheat_eos` and `battleye`.
- Bottle Doctor source policies can now report a game-local installer asset path for those components.
- Component repair resolves game-local EAC/BattlEye assets before reporting an asset as missing.
- EAC EOS `.bat` installers are parsed into direct `EasyAntiCheat_EOS_Setup.exe install <product-id>` calls so the repair path avoids script pauses.
- Runtime asset paths are canonicalized before repair because Wine rejects doubled `//Volumes//...` paths for executable launch.

For BattlEye:

- `BERCon.exe` is only an RCon client
- real proof needs a Steam title that ships BattlEye
- inspect game folder for `BEService.exe`, `BEClient*.dll`, launcher bootstrap, and service install commands
- test what fails: service creation, driver expectation, network auth, Unix runtime absence, or vendor block
- current AverySSD Steam scan has not found a real BattlEye service/runtime payload yet

For EAC:

- Elden Ring is the first local game proof target because it ships `EasyAntiCheat/easyanticheat_eos_setup.exe`
- Rubicon is the first completed EAC EOS install proof: it ships EAC EOS, not BattlEye, and `EasyAntiCheat_EOS_Setup.exe install 789399aada914e66bb3c3facebc5d709` completed successfully in `~/.metalsharp/prefix-steam`
- dry-run component repair before attempting an actual service install
- verify whether Unix EAC assets exist before treating online anti-cheat as supportable
- compare what Proton expects versus what macOS/Wine can provide

## Phase 10: UI Productization

The Sharp Library should show understandable state:

- Installing Windows Program
- Installed Apps
- Installer Logs
- Runtime Components
- Repair
- Import App
- Relaunch Installer
- Open Bottle

Failed cards should show human-readable failure summaries, for example:

```text
EA App install failed at MSI stage.
Error: INST-14-1603 / 0x80070643
Likely runtime: .NET custom action or MSI service behavior.
```

## Immediate Execution Order

1. Fresh EA bottle with the bare-Wine/WebView/`dotnet48` route.
2. Inspect whether the direct MSI log now has content.
3. Ubisoft Connect fresh proof installed launcher files, detected `UbisoftConnect.exe`, then hit the crash-reporter path with `corefonts` and `webview2` still pending.
4. Elden Ring EAC EOS dry-run repair resolves the game-local setup executable through `steam_1245620`.
5. Rubicon EAC EOS actual repair completed successfully in `steam_1888160`; launch Rubicon next and capture whether failure moves to EAC bootstrap, offline mode, or online/vendor block.
6. Repair Ubisoft `corefonts` and `webview2`, relaunch `UbisoftConnect.exe`, and capture whether the next failure is rendering, service/elevation, or auth.
7. Run Battle.net or Epic after that.
8. Save BattlEye for a real Steam title, because Rubicon and Elden Ring are EAC EOS targets and `BERCon.exe` is not the runtime installer.

## Implementation Notes

- `POST /sharp-library/install` accepts `freshBottle: true` for proof runs that must avoid stale prefix state from a previous run of the same installer path.
- `POST /launcher/evidence` collects launcher-installer proof for EA and Ubisoft bottles. It reports installer launch logs, launcher-specific logs, detected launcher executables, known EA MSI failures, Ubisoft crash-reporter evidence, and whether a direct post-install launcher launch has actually been recorded.
- Steam game runtime asset detection now covers `_CommonRedist`, `installscript.vdf`, `EasyAntiCheat`, `EasyAntiCheat_EOS`, `BEService`, `BEClient`, and `BEDaisy` markers.
- Anti-cheat proof should begin with Doctor/dry-run evidence, because running service installers can leave stale Wine service state behind.
