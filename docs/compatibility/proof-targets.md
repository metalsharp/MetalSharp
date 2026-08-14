# MetalSharp Proof Targets
**Updated:** 2026-07-08


Status: Phase 11 active

The roadmap has to be driven by reproducible evidence. `configs/proof-targets.json` is the starting ledger for installer, launcher, Steam runtime, and anti-cheat proof targets.

## Target Groups

- Installer baseline: Minecraft Launcher, GOG offline installer, Itch/Unity demo, Unreal demo.
- Launcher baseline: EA App, Ubisoft Connect, Battle.net, Epic Games Launcher.
- Steam runtime baseline: known DXMT, VKD3D, Steam CommonRedist, and Steam third-party launcher cases.
- Anti-cheat baseline: EAC title with Proton support, BattlEye title with Proton support, policy-blocked title, Windows-kernel-only title.

## Evidence Fields

Every target should record:

- source
- appid, installer path, bottle id, compatdata path, runtime profile, launch pipeline, and prefix path
- install result
- launcher open result
- login/session result
- child game process result
- game launch result
- graphics route
- audio status
- input status
- online/session status
- anti-cheat status
- failure classification
- failure evidence paths
- next action

## Rule

Do not mark a target as working from vibes. A target moves forward only when the matching bottle log, compatdata record, Launch Doctor report, crash log, or screenshot exists.

## Phase 11 Run Log

### 2026-05-19: Minecraft Launcher installer

Route used:

```text
POST /sharp-library/install {"srcPath":"~/Downloads/MinecraftInstaller.exe","name":"Minecraft Launcher Phase 11"}
```

Observed result:

- MetalSharp created/reused bottle `installer_057475b8830b64bc`.
- The bottle manifest classifies the target as `runtime_profile=java_launcher`, `installer_kind=java`, `arch=win32`.
- The launcher route started on the legacy D3D9 pipeline.
- The process exited quickly and no final launcher app was detected.
- Bottle Doctor reports the prefix exists, but tracked components are not ready and app detection is still empty.
- A manual bare-Wine rerun in the same bottle reproduces the same native crash, so this is not only a graphics-route issue.
- A manual rerun with `WINEDLLOVERRIDES=mscompatdb=d` still reproduces the same native crash.
- `gecko` repair launched, logged, and is now detected from Wine's `system32/gecko` / `syswow64/gecko` locations.
- `corefonts` is now repaired by mapping locally installed host fonts into the bottle font directory.
- `webview2` remains a missing local runtime asset.
- A rerun after font/Gecko cleanup still exits with the same `mscompatdb` / Mono native crash.
- The launch log records repeated `mscompatdb` load attempts followed by a native crash report in the Mono/native runtime path.

Evidence:

- `~/.metalsharp/bottles/installer_057475b8830b64bc/bottle.json`
- `~/.metalsharp/bottles/installer_057475b8830b64bc/logs/launch-1779249524.log`
- `~/.metalsharp/bottles/installer_057475b8830b64bc/logs/launch-1779250143.log`
- `~/.metalsharp/bottles/installer_057475b8830b64bc/logs/manual-winebare-1779249756.log`
- `~/.metalsharp/bottles/installer_057475b8830b64bc/logs/manual-disable-mscompatdb-1779250267.log`
- `~/.metalsharp/bottles/installer_057475b8830b64bc/logs/component-corefonts-1779250127.log`
- `~/.metalsharp/bottles/installer_057475b8830b64bc/logs/component-gecko-1779249576.log`
- `POST /bottles/doctor {"id":"installer_057475b8830b64bc"}`

Failure classification:

- `runtime_bug`
- `missing_runtime_asset`

Next action:

Fix the Wine Mono/mscompatdb crash and map WebView2 assets, then rerun the installer through the same bottle. This is now a concrete Phase 11 blocker rather than a generic "installer.exe does not launch" report.

### 2026-05-19: Minecraft Launcher legacy MSI route

Route used:

```text
POST /sharp-library/install {"srcPath":"~/.metalsharp/runtime/redist/Minecraft/MinecraftInstaller.msi","name":"Minecraft Launcher Legacy MSI"}
POST /sharp-library/import-bottle-app {"bottleId":"installer_6a0a76294c1d1364","exePath":".../MinecraftLauncher.exe","name":"Minecraft Launcher"}
POST /sharp-library/launch {"id":"bottle_app_ccf06adb8b050608","engine":"wine_bare"}
```

Observed result:

- The official Mojang MSI installed successfully into bottle `installer_6a0a76294c1d1364`.
- The installed launcher was detected at `C:\Program Files (x86)\Minecraft Launcher\MinecraftLauncher.exe`.
- The launcher created bottle-local state under `C:\users\alexmondello\AppData\Roaming\.minecraft`.
- CEF initialized successfully, but the initial launcher window rendered blank.
- General CEF compatibility was then applied by preserving `MinecraftLauncher_real.exe` and replacing `MinecraftLauncher.exe` with an architecture-matched wrapper.
- The wrapper launches the real executable with `--in-process-gpu --disable-gpu`, matching the working Steam CEF strategy.
- The wrapper also deploys `metalsharp-cefchildhook.dll`; local hook logs show it loads and patches launcher/module `CreateProcessA/W`, `GetProcAddress`, and `ShellExecute` imports.
- The wrapped launcher still renders blank. After clearing Minecraft's `launch_attempts.json` retry guard, logs show CEF initialization, successful network calls, successful XAL token initialization, and the main window opening.
- The remaining blocker is now lower than top-level wrapping: Minecraft logs renderer/GPU subprocess command lines from `MinecraftLauncher_real.exe`, but those launches are not yet passing through the hooked process creation or shell execution paths.
- Minecraft's launcher binary exposes `disableGPU`, `disableGPUCommandLine`, `disableGPUForced`, and `additionalCEFOptions` strings. A bottle-local settings JSON attempt was accepted but did not appear in emitted CEF child command lines.

Evidence:

- `~/.metalsharp/bottles/installer_6a0a76294c1d1364/bottle.json`
- `~/.metalsharp/bottles/installer_6a0a76294c1d1364/logs/launch-1779252105.log`
- `~/.metalsharp/bottles/installer_6a0a76294c1d1364/logs/launch-1779252174.log`
- `~/.metalsharp/bottles/installer_6a0a76294c1d1364/prefix/drive_c/users/alexmondello/AppData/Roaming/.minecraft/launcher_cef_log.txt`
- `~/.metalsharp/bottles/installer_6a0a76294c1d1364/prefix/drive_c/Program Files (x86)/Minecraft Launcher/.ms_cef_compat_MinecraftLauncher`
- `~/.metalsharp/bottles/installer_6a0a76294c1d1364/prefix/drive_c/users/alexmondello/AppData/Local/Temp/metalsharp-cefchildhook.log`

Failure classification:

- `store_bootstrapper_mismatch` for the Microsoft Store `.exe` route
- `cef_rendering_path` for the blank launcher window after MSI install
- `cef_child_creation_path` for the post-install blank UI state

Next action:

Finish the Minecraft CEF child-process path by either reaching the lower-level process creation call or mapping Minecraft's native CEF preference surface correctly, then use the working MSI/bottle install to continue toward starting Minecraft Java from the same bottle.

Mono follow-up:

- See `docs/runtime/mono-runtime-lanes.md`.
- Keep Minecraft in the Wine-bottle lane for now because it is a Windows launcher/bootstrapper, not a native FNA game.
- Use the old Terraria/Celeste native Mono lanes as selective fallback profiles for known FNA/XNA games, not as a global replacement for Wine Mono.

### 2026-05-19: EA App installer

Route used:

```text
POST /sharp-library/install {"srcPath":"~/Downloads/EAappInstaller.exe","name":"EA App"}
POST /bottles/repair-component {"id":"installer_16c2e7d7a6e2d5e7","component":"dotnet48"}
POST /bottles/relaunch-installer {"id":"installer_16c2e7d7a6e2d5e7"}
```

Observed result:

- MetalSharp created bottle `installer_16c2e7d7a6e2d5e7`.
- The classifier mapped EA to `runtime_profile=webview`.
- Before this fix, the 32-bit PE fallback still launched the known launcher through a graphics route.
- The EA bootstrapper downloaded and verified `EAapp-13.700.0.6213-4218.msi`.
- After the visible install bar completed, the MSI failed with `0x80070643`.
- EA reports that MSI failure as `INST-14-1603`.
- The extracted MSI payload includes `Microsoft.Deployment.WindowsInstaller.dll` and `CustomAction.config` with `<supportedRuntime version="v4.0" />`, so EA is running .NET v4 custom actions during install.
- Installing `dotnet48` repaired the bottle component state, but the relaunch still reproduced `INST-14-1603`.
- A fresh proof bottle, `installer_16c2e7d7a6e2d5e7_fresh_1779256322130`, confirmed the relaunch path now stays inside the selected proof bottle instead of falling back to the stable source-path bottle.
- In the fresh proof bottle, `corefonts`, `dotnet48`, `gecko`, `vcrun2019`, and `webview2` are installed.
- The local WebView2 resolver now honors `~/.metalsharp/runtime/redist/WebView2/`, matching the documented redist path.
- Even with WebView2 present, the direct MSI log files are created but remain zero bytes, so Wine/MSI appears to fail before the MSI logger records the real custom-action body.
- The outer WiX bootstrapper still maps the failure to `0x80070643 aka 'INST-14-1603'`.

Evidence:

- `~/.metalsharp/bottles/installer_16c2e7d7a6e2d5e7/bottle.json`
- `~/.metalsharp/bottles/installer_16c2e7d7a6e2d5e7/prefix/drive_c/users/alexmondello/AppData/Local/Temp/EA_app_20260519233518.log`
- `~/.metalsharp/bottles/installer_16c2e7d7a6e2d5e7/prefix/drive_c/users/alexmondello/AppData/Local/Temp/EA_app_20260519233838.log`
- `~/.metalsharp/bottles/installer_16c2e7d7a6e2d5e7/prefix/drive_c/users/alexmondello/AppData/Local/Temp/msi56c0.tmp-/CustomAction.config`
- `~/.metalsharp/bottles/installer_16c2e7d7a6e2d5e7_fresh_1779256322130/bottle.json`
- `~/.metalsharp/bottles/installer_16c2e7d7a6e2d5e7_fresh_1779256322130/prefix/drive_c/users/alexmondello/AppData/Local/Temp/EA_app_20260519235554.log`
- `~/.metalsharp/bottles/installer_16c2e7d7a6e2d5e7_fresh_1779256322130/prefix/drive_c/users/alexmondello/AppData/Local/Temp/EA_app_20260519235554_000_EAapp_13.700.0.6213_ae714772e_4bfd0163_4218.msi.log`
- `~/.metalsharp/bottles/installer_16c2e7d7a6e2d5e7_fresh_1779256322130/prefix/drive_c/users/alexmondello/AppData/Local/Temp/EA_app_20260520000339.log`
- `~/.metalsharp/bottles/installer_16c2e7d7a6e2d5e7_fresh_1779256322130/logs/component-webview2-1779256915.log`
- `~/.metalsharp/bottles/installer_16c2e7d7a6e2d5e7_fresh_1779256322130/prefix/drive_c/users/alexmondello/AppData/Local/Temp/msi2e9e.tmp-/CustomAction.config`

Failure classification:

- `ea_inst_14_1603`
- `msi_custom_action_failure`
- `launcher_bootstrapper_needs_bare_wine`
- `webview_profile_needs_dotnet48`
- `msi_log_empty`
- `webview2_installed_not_sufficient`

Next action:

Inspect Wine MSI custom-action service/elevation behavior around per-machine package cache writes. WebView2 is now installed in the proof bottle, so the blocker is below the missing-runtime layer.

### 2026-05-19: BattlEye BERCon artifact

Observed result:

- `~/Downloads/BERCon.exe` is a PE32 console app, not a service installer.
- Embedded strings identify it as `BattlEye RCon v0.94 beta`.
- The binary exposes remote-console command-line help: `BERCon [-host IP ADDRESS / HOSTNAME] [-port PORT] [-pw PASSWORD]`.
- PE imports are limited to console/process basics plus `WS2_32.dll`; there is no visible `BEService`, driver, service-install, or anti-cheat runtime payload in this artifact.

Evidence:

- `file ~/Downloads/BERCon.exe`
- `shasum -a 256 ~/Downloads/BERCon.exe`
- `strings -a ~/Downloads/BERCon.exe`
- `i686-w64-mingw32-objdump -p ~/Downloads/BERCon.exe`

Failure classification:

- `artifact_mismatch`

Next action:

Do not use BERCon as proof that the BattlEye runtime is installed. Pick a real Steam title that ships BattlEye/BEService assets, then capture the launch-time service behavior from the game bottle and Launch Doctor.

### 2026-05-20: Ubisoft Connect installer artifact

Observed result:

- `~/Downloads/UbisoftConnectInstaller.exe` is a PE32 GUI installer.
- `file` identifies it as a Nullsoft Installer self-extracting archive.
- SHA-256: `7a6942ef2c96ed516b4e06256b03ac0bb994de22702e944f4ebf2c2507f31a24`
- Fresh proof bottle: `~/.metalsharp/bottles/installer_61e534a6d260c814_fresh_1779258414613`
- Install completed enough to stage `UbisoftConnect.exe` under `Program Files (x86)/Ubisoft/Ubisoft Game Launcher/`.
- `POST /launcher/evidence {"family":"ubisoft"}` reports `ubisoft_auto_started_then_crash_reporter`.
- The installer auto-started Ubisoft Game Launcher `171.0.13174`.
- There is not yet a recorded direct post-install launch of `UbisoftConnect.exe`; the next proof pass should launch that detected executable directly after runtime repair.
- The launcher auto-started as `upc.exe uplay://open`, then `UplayCrashReporter.exe` appeared.
- Bottle Doctor still reports missing `corefonts` and `webview2`.
- App detection initially surfaced launcher plumbing (`UplayService`, `UplayWebCore`, `UpcElevationService`, `upc.exe`), so the filter now excludes those helpers and preserves `UbisoftConnect.exe`.

Failure classification:

- `nsis_bootstrapper`
- `launcher_started_then_crash_reporter`
- `webview2_missing_after_install`
- `app_detection_helper_filter`

Evidence:

- `~/.metalsharp/bottles/installer_61e534a6d260c814_fresh_1779258414613/logs/launch-1779258414.log`
- `~/.metalsharp/bottles/installer_61e534a6d260c814_fresh_1779258414613/prefix/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/logs/launcher_log.txt`
- `~/.metalsharp/bottles/installer_61e534a6d260c814_fresh_1779258414613/prefix/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/logs/client_crash_reporter.txt`

Next action:

Repair `corefonts` and `webview2` in the fresh proof bottle, relaunch `UbisoftConnect.exe` directly, then compare the crash path against EA's `INST-14-1603`. The useful signal is whether Ubisoft fails at WebView/CEF rendering, service/elevation, or post-install launcher launch.

### 2026-05-20: EA and Ubisoft launcher evidence endpoint

MetalSharp now has a launcher evidence report:

```http
POST /launcher/evidence
{"family":"ea"}

POST /launcher/evidence
{"family":"ubisoft"}
```

Current EA status:

- `ea_msi_1603`
- EA reaches MSI apply and fails with `0x80070643 / INST-14-1603`.
- The failing package is `EAapp_13.700.0.6213_ae714772e_4bfd0163_4218.msi`.
- Direct EA launcher launch is not expected yet because no launcher executable was installed.

Current Ubisoft status:

- `ubisoft_auto_started_then_crash_reporter`
- `UbisoftConnect.exe` exists under the proof bottle.
- The installer auto-started Ubisoft Game Launcher `171.0.13174`.
- Crash reporter evidence is present.
- No direct post-install launch of `UbisoftConnect.exe` has been recorded yet.

### 2026-05-20: Game-local anti-cheat installer scan

Observed local Steam installs:

- `/Volumes/AverySSD/SteamLibrary/steamapps/common/ELDEN RING/Game/EasyAntiCheat/easyanticheat_eos_setup.exe`
- `/Volumes/AverySSD/SteamLibrary/steamapps/common/ELDEN RING/Game/EasyAntiCheat/install_easyanticheat_eos_setup.bat`

The Elden Ring install script runs:

```text
EasyAntiCheat_EOS_Setup.exe install 773d3a68f76f4b2ebebc5b4127bbad3e
```

No real BattlEye service/runtime payload was found in the current AverySSD Steam library scan. `BERCon.exe` remains excluded because it is an RCon client, not a service installer.

Implementation result:

- Steam game bottle runtime asset detection now sees EAC/BattlEye assets outside `_CommonRedist`.
- Bottle Doctor can report those assets as `game_runtime_asset` component sources.
- Component repair can resolve game-local EAC/BattlEye installers instead of looking only in global redistributable folders.

Next action:

Use dry-run repair against `steam_1245620` first, then attempt an actual EAC EOS install only after the Doctor resolves the expected local asset path.

### 2026-05-20: Rubicon EAC EOS proof

Observed local Steam install:

- Appid: `1888160`
- Game: `ARMORED CORE VI FIRES OF RUBICON`
- Install path: `/Volumes/AverySSD/SteamLibrary/steamapps/common/ARMORED CORE VI FIRES OF RUBICON`

Important correction:

- Rubicon ships Easy Anti-Cheat EOS, not BattlEye.
- No `BEService`, `BEClient`, `BEDaisy`, or BattlEye install script was found in the Rubicon game folder.

Detected EAC assets:

- `/Volumes/AverySSD/SteamLibrary/steamapps/common/ARMORED CORE VI FIRES OF RUBICON/Game/EasyAntiCheat/easyanticheat_eos_setup.exe`
- `/Volumes/AverySSD/SteamLibrary/steamapps/common/ARMORED CORE VI FIRES OF RUBICON/Game/EasyAntiCheat/install_easyanticheat_eos_setup.bat`
- `/Volumes/AverySSD/SteamLibrary/steamapps/common/ARMORED CORE VI FIRES OF RUBICON/Game/EasyAntiCheat/eacchecker.bat`

The install script runs:

```text
EasyAntiCheat_EOS_Setup.exe install 789399aada914e66bb3c3facebc5d709
```

Implementation result:

- The first dry-run exposed a resolver bug: it picked `eacchecker.bat` before the real install script.
- Resolver now parses EAC scripts and only accepts a script when it contains a real `EasyAntiCheat_EOS_Setup.exe install <product-id>` command.
- Runtime asset paths are canonicalized before storage, because Wine rejected `//Volumes//...` installer paths with `ShellExecuteEx failed: File not found`.
- After normalization, `steam_1888160` dry-run repair resolved the canonical EAC setup executable.
- Actual repair started `easyanticheat_eos_setup.exe install 789399aada914e66bb3c3facebc5d709` in `~/.metalsharp/prefix-steam`.
- EAC service log reports an elevation relaunch and `Operation 1 completed successfully`.
- `~/.metalsharp/prefix-steam/drive_c/Program Files (x86)/EasyAntiCheat_EOS/EasyAntiCheat_EOS.exe` now exists.
- Bottle Doctor for `steam_1888160` reports `Bottle runtime checks passed`.

Evidence:

- `~/.metalsharp/bottles/steam_1888160/logs/component-easyanticheat_eos-1779259468.log`
- `~/.metalsharp/bottles/steam_1888160/logs/component-easyanticheat_eos-1779259573.log`
- `~/.metalsharp/prefix-steam/drive_c/users/alexmondello/AppData/Roaming/EasyAntiCheat/service.log`

Next action:

Launch Rubicon through the Steam game bottle and capture whether the game reaches EAC bootstrap, offline mode, or an online/vendor block.

### 2026-05-21: Protected EAC module mapping proof

Read-only anti-cheat endpoint probes were run against the completed AverySSD Steam installs for:

- `1245620` / `ELDEN RING`
- `1888160` / `ARMORED CORE VI FIRES OF RUBICON`

Install evidence:

- Both manifests report `StateFlags 4`.
- Both protected launchers and game executables have valid `PE` headers.
- Both game folders contain game-local `EasyAntiCheat/Settings.json` plus `easyanticheat_eos_setup.exe`.

Important diagnostic correction:

- The shared Wine Steam prefix can contain EAC launcher logs for multiple games.
- Anti-cheat evidence must be scoped by appid and the game-local EAC `productid` / `deploymentid`.
- Without that filter, Elden Ring probes can accidentally summarize Rubicon's EAC launcher log.

Current protected-launch status:

- Elden Ring resolves to product `773d3a68f76f4b2ebebc5b4127bbad3e` and deployment `d2842e93d53b4c0c98a8f963ebb4c222`.
- Rubicon resolves to product `789399aada914e66bb3c3facebc5d709` and deployment `b978a2afd2254108bbb39201d0a24a98`.
- Both titles independently select EAC module target `linux64`.
- Both titles reach Wine module mapping under `wine-11.5`.
- Both titles fail with launcher exit `206` and `Failed to load the anti-cheat module`.

Current substrate decision:

- `linux_module_on_darwin_boundary`
- `requires_linux_user_space_substrate_or_vendor_macos_asset`

Next action:

Prototype harmless loader/runtime contract probes for the Linux user-space boundary before changing protected launch behavior.

Implementation follow-up:

- `POST /steam/anticheat-contract-probe` records a harmless host contract probe.
- The probe uses synthetic temporary data only; it does not load protected anti-cheat modules.
- The probe reports memory mapping/protection behavior, synthetic ELF direct-load behavior, Wine loader/wineserver state, and the scoped game EAC identity.
- For both Elden Ring and Rubicon, the expected macOS status is `linux_elf_host_gap_confirmed` until MetalSharp has either a truthful Linux user-space substrate or vendor-supported macOS module assets.

Local probe result:

- Elden Ring and Rubicon both return `linux_elf_host_gap_confirmed`.
- Anonymous memory can transition from writable to executable in the backend process.
- The host dynamic loader rejects a synthetic ELF `.so` as not valid Mach-O.
- This narrows the next implementation target to the Linux ELF module host/substrate boundary rather than M11/VKD3D graphics routing.
