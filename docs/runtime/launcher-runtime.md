# MetalSharp Launcher Runtime
**Updated:** 2026-07-08


Status: Phase 3 foundation

MetalSharp treats launcher installers as bottle-managed Windows programs, not as one-off EXEs. The goal is to let launchers keep their login/session state, install child games into the same bottle, and produce logs that explain why a launcher or child game failed.

## Known Launcher Recipes

The installer classifier recognizes these launcher families before generic .NET, WebView, MSI, or PE import heuristics:

- Minecraft Launcher -> Java Launcher profile
- EA App / Origin -> WebView profile
- Ubisoft Connect / Uplay -> WebView profile
- Battle.net / Blizzard launcher -> WebView profile
- Epic Games Launcher -> WebView profile
- Rockstar Games Launcher / Social Club -> WebView profile
- GOG Galaxy -> Launcher profile

Known launcher hints are stored in the classifier output as `known_launcher:<id>` and `launcher_name:<display name>`. These hints make installer bottles more predictable, especially when launcher bootstrapper binaries also contain generic .NET or WebView strings.

Known launchers default to the bare Wine pipeline during install/bootstrap. That keeps store launchers from inheriting game-specific graphics routes before the actual child game executable exists. Once a launcher installs or starts a game, that child executable still gets its own bottle/runtime route.

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
- known launchers now install through bare Wine first instead of falling back to a graphics route from the 32-bit bootstrapper PE header
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
