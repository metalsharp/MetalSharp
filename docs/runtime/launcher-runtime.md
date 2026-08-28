# MetalSharp Launcher Runtime

**Updated:** 2026-08-25

Status: Phase 3 foundation

MetalSharp treats launcher installers as bottle-managed Windows programs, not as one-off EXEs. The goal is to let launchers keep their login/session state, install child games into the same bottle, and produce logs that explain why a launcher or child game failed.

## Known Launcher Recipes

The installer classifier recognizes these launcher families before generic .NET, WebView, MSI, or PE import heuristics:

- Minecraft Launcher -> Java Launcher profile
- EA App / Origin -> WebView profile
- Ubisoft Connect / Uplay -> WebView profile
- Epic Games Launcher -> WebView profile
- Rockstar Games Launcher / Social Club -> WebView profile
- GOG Galaxy -> Launcher profile

Known launcher hints are stored in the classifier output as `known_launcher:<id>` and `launcher_name:<display name>`. These hints make installer bottles more predictable, especially when launcher bootstrapper binaries also contain generic .NET or WebView strings.

Known launchers default to the bare Wine pipeline during install/bootstrap. That keeps store launchers from inheriting game-specific graphics routes such as M9 before the actual child game executable exists. Once a launcher installs or starts a game, that child executable still gets its own bottle/runtime route.

## Native Epic Library Path

The Sharp Library **Epic** tab is the supported game-download path when the Windows launcher reaches `DP-06`. It uses upstream [Legendary](https://github.com/legendary-gl/legendary) 0.21.0 out of process rather than patching Wine, ADVAPI32, NTDLL, or Epic binaries.

- MetalSharp downloads the official native arm64 release only after the user selects **Install Epic Support**.
- The release URL, version, 64 MiB size ceiling, arm64 Mach-O identity, and SHA-256 `28f5f7d0eb8c029679d4faaa483ec85888af17a9a75977ae9170c21d8ce3428b` are backend-owned and verified before atomic activation at `~/.metalsharp/tools/legendary/legendary-0.21.0`.
- Epic authentication occurs in a sandboxed, context-isolated Electron window restricted to Epic, Legendary, and explicit identity-provider HTTPS hosts. The backend receives only the resulting authorization code. Legendary state is isolated under `~/.metalsharp/epic/legendary/`.
- Library sync requests Windows-installable account entries as JSON on login and manual **Sync**. The backend also starts a serialized background sync when it launches. Successful catalogs are atomically cached at `~/.metalsharp/epic/library.json`, and normal Epic-tab loads read that cache immediately so navigation never clears the library while a network refresh is running or unavailable.
- Downloads use Legendary's manifest/CDN pipeline. Each install prompts for an existing writable location constrained to the user's home or `/Volumes`; the configured root from `~/.metalsharp/launcher-games/epic/location.txt` remains the backend fallback. Per-title progress and logs live under `~/.metalsharp/epic/processes/`.
- Installed games require an explicit **Initialize Bottle** action before first launch. Each title owns `~/.metalsharp/bottles/epic_<appName>/prefix` plus a managed manifest recording its selected pipeline and mouse mode. **No Recenter** is the default; **Mouse Auto** restores Wine cursor warping for games that need relative capture.
- Pipeline and mouse selectors remain beside Play/Uninstall like the GOG library. Launches inherit the selected MetalSharp graphics backend, and supervision waits on the prefix's real Wineserver. Stop, the card close action, and **Cmd+Opt+Q** terminate the isolated Epic Wineserver. Uninstall removes Legendary's registered game files and that title's bottle.
- Epic account data, the cached catalog, configured game location, per-game bottle manifests, and each Epic bottle's registry/user settings are explicitly preserved and restored by runtime migration.
- MetalSharp does not use or redistribute CrossOver code or binaries for this path. The unsupported Windows Epic Launcher card and its legacy `Epic-Games-Prefix` have been removed.

Backend routes are `GET /sharp-library/epic/status`, `GET /sharp-library/epic/games`, and POST actions for `install-tool`, `auth`, `logout`, `sync`, `install`, `progress`, `cancel`, `initialize`, `play`, `stop`, `stop-all`, and `uninstall`.

## Runtime Behavior

`Install Windows Program` routes launcher-like EXEs and MSI packages into installer bottles. The bottle records:

- source installer path
- installer kind
- runtime profile
- prefix path
- launch log
- launch pid/status
- detected installed app candidates

Minecraft gets a Java Launcher profile even when the bootstrapper includes CLR metadata. Storefront launchers get WebView or Launcher profiles so their bottle dependency set matches the login and embedded-browser surface they actually need. The WebView profile includes Gecko, WebView2, .NET 4.8, VC runtime, and core fonts because EA-style WiX/MSI launchers can execute .NET custom actions after the visible install bar completes.

## CEF Compatibility

Steam already uses a wrapped `steamwebhelper.exe` to force CEF onto a Wine-safe software GPU path. Sharp Library bottles now generalize that behavior for launcher apps that carry CEF or Chromium payloads.

When a bottle app looks like a launcher and its install directory contains CEF assets such as `libcef.dll`, `chrome_*.pak`, `vk_swiftshader.dll`, or `app.asar`, MetalSharp preserves the original executable as `<name>_real.exe` and replaces `<name>.exe` with a small architecture-matched wrapper. The wrapper relaunches the real executable with `--in-process-gpu --disable-gpu` and deploys a sibling `metalsharp-cefchildhook.dll` for launchers that spawn renderer, utility, or GPU children from the preserved executable.

The first proof target is Minecraft Launcher:

- the Microsoft Store `.exe` bootstrapper is a 32-bit .NET/WPF package and can fall into Wine Mono or native .NET setup failure before Minecraft exists
- the official Mojang `MinecraftInstaller.msi` installs cleanly into a `java_launcher` bottle
- `MinecraftLauncher.exe` now receives the generic CEF wrapper and child hook, but the current proof still renders a blank surface after CEF initializes
- local hook logs prove imports are patched, but Minecraft's embedded CEF child creation is not yet passing through the hooked `CreateProcessA/W`, `GetProcAddress`, or `ShellExecute` paths

EA App is the first Steam-adjacent storefront proof target:

- the installer reaches the EA MSI apply step in bottle `installer_16c2e7d7a6e2d5e7`
- the visible install bar completes, then the MSI fails with `0x80070643`, which EA reports as `INST-14-1603`
- extracted MSI custom-action metadata requests `.NET v4.0`, so the WebView profile now provisions `dotnet48` before the launcher installer runs
- known launchers now install through bare Wine first instead of falling back to M9 from the 32-bit bootstrapper PE header
- fresh proof bottle relaunches now stay in the selected proof bottle instead of silently falling back to the stable source-path bottle
- the latest EA proof has `corefonts`, `dotnet48`, `gecko`, `vcrun2019`, and `webview2` installed
- the direct MSI log files are still created as zero bytes, so the next EA pass needs deeper Wine MSI/service/elevation inspection around per-machine package cache writes
- WebView2/Edge helper executables are runtime components, not apps; prefix app detection filters them so a runtime repair does not pollute the Sharp Library

Launcher evidence reports:

```http
POST /launcher/evidence
{"family":"ea"}

POST /launcher/evidence
{"family":"ubisoft"}
```

EA currently reports `ea_msi_1603`: the bootstrapper reaches MSI apply and fails with `0x80070643 / INST-14-1603`.

Ubisoft currently reports `ubisoft_auto_started_then_crash_reporter`: the installer staged `UbisoftConnect.exe`, auto-started Ubisoft Game Launcher `171.0.13174`, then entered the crash-reporter path. A clean direct launch of `UbisoftConnect.exe` still needs to be captured after runtime repair.

## Remaining Work

- Finish the Minecraft CEF child-process path by either reaching the lower-level process creation call or mapping Minecraft's native CEF preference surface correctly.
- Persist child game processes spawned by launchers as bottle app records.
- Track launcher-owned game install folders separately from the launcher EXE.
- Add launcher-specific repair controls for WebView, Gecko, VC runtime, and session data.
- Add end-to-end smoke cases for at least three launchers once redistributable assets and test installers are available.
